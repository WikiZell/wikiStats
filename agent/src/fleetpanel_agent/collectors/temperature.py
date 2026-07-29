"""Temperature collection with a layered source strategy.

Sources are tried in order and merged; the first source that yields any reading wins
for the ``temperatures`` list, but ``vcgencmd`` is always consulted on a Pi because
its SoC reading is the one users recognise.

Order:

1. ``psutil.sensors_temperatures()`` - covers x86 ``coretemp``/``k10temp`` and most
   ARM boards that expose hwmon.
2. ``/sys/class/thermal/thermal_zone*`` - covers boards whose hwmon nodes are absent.
3. ``vcgencmd measure_temp`` - Raspberry Pi VideoCore firmware.
4. ``sensors -j`` - opt-in, for exotic hwmon layouts psutil does not decode.

Nothing here raises: a host without sensors returns ``(None, [])`` and the caller
drops the ``cpu_temperature`` capability.
"""

from __future__ import annotations

import json
import logging
import re
import subprocess
from collections.abc import Iterable, Sequence
from pathlib import Path
from typing import Any

import psutil

from ..logging_setup import get_logger, log_throttled

_log = get_logger("temperature")

_SYSFS_THERMAL = Path("/sys/class/thermal")
_VCGENCMD_RE = re.compile(r"temp=([0-9]+(?:\.[0-9]+)?)'?C")
_SUBPROCESS_TIMEOUT = 2.0

# Plausibility window. Sensors occasionally report 0 (unpopulated) or huge values
# (raw millidegree leaking through a driver bug); both would poison the display.
_MIN_PLAUSIBLE_C = -40.0
_MAX_PLAUSIBLE_C = 150.0

DEFAULT_PREFERRED_LABELS: tuple[str, ...] = (
    "coretemp",
    "k10temp",
    "cpu_thermal",
    "soc_thermal",
    "package",
    "tctl",
    "cpu-thermal",
    "x86_pkg_temp",
    "cpu",
)


def _normalise(label: str) -> str:
    return label.strip().lower().replace(" ", "_").replace("-", "_")


def _plausible(value: float | None) -> bool:
    return value is not None and _MIN_PLAUSIBLE_C <= value <= _MAX_PLAUSIBLE_C


def _optional_c(value: float | None) -> float | None:
    """Round a limit value, or drop it when the driver reports 0/absurd/None."""
    if value is None or not value or not _plausible(value):
        return None
    return round(float(value), 1)


def _reading(
    label: str,
    current: float | None,
    high: float | None = None,
    critical: float | None = None,
) -> dict[str, Any] | None:
    if current is None or not _plausible(current):
        return None
    return {
        "label": label,
        "temperature_c": round(float(current), 1),
        "high_c": _optional_c(high),
        "critical_c": _optional_c(critical),
    }


def select_primary(
    readings: Sequence[dict[str, Any]],
    preferred: Iterable[str] = DEFAULT_PREFERRED_LABELS,
) -> float | None:
    """Pick the CPU temperature a human would quote.

    Preference is by documented label priority. Within one priority level the hottest
    reading wins, because ``coretemp`` reports one entry per core and the package
    maximum is the number that matters for throttling.
    """
    if not readings:
        return None
    prefs = [_normalise(p) for p in preferred]
    best_rank: int | None = None
    best_value: float | None = None
    for item in readings:
        raw = item.get("temperature_c")
        if raw is None or not _plausible(raw):
            continue
        value = float(raw)
        label = _normalise(str(item.get("label", "")))
        rank = len(prefs)
        for index, pref in enumerate(prefs):
            if pref in label:
                rank = index
                break
        hotter_at_same_rank = rank == best_rank and value > (
            best_value if best_value is not None else -273.0
        )
        if best_rank is None or rank < best_rank or hotter_at_same_rank:
            best_rank = rank
            best_value = value
    if best_value is None:
        return None
    return round(best_value, 1)


def read_psutil_sensors() -> list[dict[str, Any]]:
    sensors = getattr(psutil, "sensors_temperatures", None)
    if sensors is None:  # not available on this platform build
        return []
    try:
        raw = sensors()
    except Exception:  # noqa: BLE001 - psutil raises assorted OSErrors per platform
        log_throttled(_log, logging.DEBUG, "temp.psutil", "psutil.sensors_temperatures failed")
        return []
    out: list[dict[str, Any]] = []
    for chip, entries in (raw or {}).items():
        for entry in entries:
            label = getattr(entry, "label", "") or ""
            full = f"{chip} {label}".strip() if label else str(chip)
            item = _reading(
                full,
                getattr(entry, "current", None),
                getattr(entry, "high", None),
                getattr(entry, "critical", None),
            )
            if item:
                out.append(item)
    return out


def read_sysfs_thermal(root: Path = _SYSFS_THERMAL) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    try:
        zones = sorted(root.glob("thermal_zone*"))
    except OSError:
        return out
    for zone in zones:
        try:
            millidegrees = int((zone / "temp").read_text(encoding="ascii").strip())
        except (OSError, ValueError):
            continue
        try:
            zone_type = (zone / "type").read_text(encoding="ascii").strip()
        except OSError:
            zone_type = zone.name
        item = _reading(zone_type or zone.name, millidegrees / 1000.0)
        if item:
            out.append(item)
    return out


def read_vcgencmd(binary: str = "/usr/bin/vcgencmd") -> dict[str, Any] | None:
    """Raspberry Pi SoC temperature. Returns ``None`` when vcgencmd is absent."""
    if not Path(binary).exists():
        return None
    try:
        completed = subprocess.run(  # noqa: S603 - fixed argv, no shell
            [binary, "measure_temp"],
            capture_output=True,
            text=True,
            timeout=_SUBPROCESS_TIMEOUT,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        log_throttled(_log, logging.DEBUG, "temp.vcgencmd", "vcgencmd invocation failed")
        return None
    return parse_vcgencmd_output(completed.stdout)


def parse_vcgencmd_output(text: str) -> dict[str, Any] | None:
    """Parse ``temp=48.2'C`` into a reading dict. Exposed for tests."""
    match = _VCGENCMD_RE.search(text or "")
    if not match:
        return None
    return _reading("cpu_thermal (vcgencmd)", float(match.group(1)))


def read_sensors_json(binary: str = "/usr/bin/sensors") -> list[dict[str, Any]]:
    if not Path(binary).exists():
        return []
    try:
        completed = subprocess.run(  # noqa: S603 - fixed argv, no shell
            [binary, "-j"],
            capture_output=True,
            text=True,
            timeout=_SUBPROCESS_TIMEOUT,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        log_throttled(_log, logging.DEBUG, "temp.sensors", "sensors -j invocation failed")
        return []
    return parse_sensors_json(completed.stdout)


def parse_sensors_json(text: str) -> list[dict[str, Any]]:
    """Parse ``sensors -j`` output. Exposed for tests.

    ``sensors`` emits one object per chip, each containing feature objects whose keys
    look like ``temp1_input`` / ``temp1_max`` / ``temp1_crit``.
    """
    try:
        data = json.loads(text or "{}")
    except (ValueError, TypeError):
        return []
    if not isinstance(data, dict):
        return []
    out: list[dict[str, Any]] = []
    for chip, features in data.items():
        if not isinstance(features, dict):
            continue
        for feature, values in features.items():
            if not isinstance(values, dict):
                continue
            current = high = critical = None
            for key, value in values.items():
                # A tuple, not `int | float`: PEP 604 in isinstance is 3.10+.
                if not isinstance(value, (int, float)):
                    continue
                if key.endswith("_input"):
                    current = float(value)
                elif key.endswith("_max"):
                    high = float(value)
                elif key.endswith("_crit"):
                    critical = float(value)
            if current is None:
                continue
            item = _reading(f"{chip} {feature}".strip(), current, high, critical)
            if item:
                out.append(item)
    return out


class TemperatureCollector:
    """Applies the layered strategy according to configuration."""

    def __init__(
        self,
        *,
        enabled: bool = True,
        use_psutil: bool = True,
        use_sysfs: bool = True,
        use_vcgencmd: bool = True,
        use_sensors_json: bool = False,
        vcgencmd_path: str = "/usr/bin/vcgencmd",
        sensors_path: str = "/usr/bin/sensors",
        preferred_labels: Sequence[str] = DEFAULT_PREFERRED_LABELS,
    ) -> None:
        self.enabled = enabled
        self.use_psutil = use_psutil
        self.use_sysfs = use_sysfs
        self.use_vcgencmd = use_vcgencmd
        self.use_sensors_json = use_sensors_json
        self.vcgencmd_path = vcgencmd_path
        self.sensors_path = sensors_path
        self.preferred_labels = tuple(preferred_labels) or DEFAULT_PREFERRED_LABELS

    def collect(self) -> tuple[float | None, list[dict[str, Any]]]:
        if not self.enabled:
            return None, []
        readings: list[dict[str, Any]] = []
        if self.use_psutil:
            readings.extend(read_psutil_sensors())
        if not readings and self.use_sysfs:
            readings.extend(read_sysfs_thermal())
        if self.use_vcgencmd:
            pi = read_vcgencmd(self.vcgencmd_path)
            if pi and not any(r["label"] == pi["label"] for r in readings):
                readings.append(pi)
        if self.use_sensors_json:
            for item in read_sensors_json(self.sensors_path):
                if not any(r["label"] == item["label"] for r in readings):
                    readings.append(item)
        if not readings:
            log_throttled(
                _log,
                logging.INFO,
                "temp.none",
                "no temperature sensors available; reporting null",
            )
            return None, []
        return select_primary(readings, self.preferred_labels), readings
