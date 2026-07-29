"""Optional subsystems: GPU and battery.

Both are ``null`` on the great majority of monitored machines, which is exactly why
the protocol reserves a slot for them rather than letting agents invent field names
later. Detection is cheap and side-effect free:

* battery - ``psutil.sensors_battery()``.
* GPU - ``amdgpu``/``i915`` counters from sysfs, or ``nvidia-smi`` when the binary is
  present. No driver libraries are linked and nothing is installed.
"""

from __future__ import annotations

import logging
import shutil
import subprocess
from pathlib import Path
from typing import Any

import psutil

from ..logging_setup import get_logger, log_throttled

_log = get_logger("optional")

_DRM_ROOT = Path("/sys/class/drm")
_SUBPROCESS_TIMEOUT = 2.0


def collect_battery() -> dict[str, Any] | None:
    sensor = getattr(psutil, "sensors_battery", None)
    if sensor is None:
        return None
    try:
        battery = sensor()
    except Exception:  # noqa: BLE001
        log_throttled(_log, logging.DEBUG, "opt.battery", "sensors_battery failed")
        return None
    if battery is None:
        return None
    seconds = getattr(battery, "secsleft", None)
    # psutil uses sentinels for "unlimited" and "unknown"; both mean "no estimate".
    seconds_remaining = None if seconds is None or seconds < 0 else int(seconds)
    return {
        "percent": round(float(battery.percent), 1),
        "power_plugged": bool(battery.power_plugged) if battery.power_plugged is not None else None,
        "seconds_remaining": seconds_remaining,
    }


def parse_nvidia_smi(text: str) -> dict[str, Any] | None:
    """Parse one CSV line of ``nvidia-smi`` query output. Exposed for tests."""
    line = (text or "").strip().splitlines()
    if not line:
        return None
    parts = [p.strip() for p in line[0].split(",")]
    if len(parts) < 5:
        return None

    def _num(value: str) -> float | None:
        try:
            return float(value)
        except ValueError:
            return None

    usage = _num(parts[1])
    temp = _num(parts[2])
    mem_total = _num(parts[3])
    mem_used = _num(parts[4])
    return {
        "name": parts[0] or None,
        "usage_percent": round(usage, 1) if usage is not None else None,
        "temperature_c": round(temp, 1) if temp is not None else None,
        "memory_total_bytes": int(mem_total * 1024 * 1024) if mem_total is not None else None,
        "memory_used_bytes": int(mem_used * 1024 * 1024) if mem_used is not None else None,
    }


def collect_nvidia_gpu() -> dict[str, Any] | None:
    binary = shutil.which("nvidia-smi")
    if not binary:
        return None
    try:
        completed = subprocess.run(  # noqa: S603 - fixed argv, no shell
            [
                binary,
                "--query-gpu=name,utilization.gpu,temperature.gpu,memory.total,memory.used",
                "--format=csv,noheader,nounits",
            ],
            capture_output=True,
            text=True,
            timeout=_SUBPROCESS_TIMEOUT,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        log_throttled(_log, logging.DEBUG, "opt.nvidia", "nvidia-smi invocation failed")
        return None
    if completed.returncode != 0:
        return None
    return parse_nvidia_smi(completed.stdout)


def collect_sysfs_gpu(root: Path = _DRM_ROOT) -> dict[str, Any] | None:
    """AMD (and some Intel) GPUs expose busy percent and VRAM through sysfs."""
    try:
        cards = sorted(root.glob("card[0-9]"))
    except OSError:
        return None
    for card in cards:
        device = card / "device"
        busy_file = device / "gpu_busy_percent"
        if not busy_file.exists():
            continue
        try:
            usage = float(busy_file.read_text(encoding="ascii").strip())
        except (OSError, ValueError):
            continue

        def _read_int(path: Path) -> int | None:
            try:
                return int(path.read_text(encoding="ascii").strip())
            except (OSError, ValueError):
                return None

        temp_raw = _read_int(device / "hwmon" / "hwmon0" / "temp1_input")
        return {
            "name": card.name,
            "usage_percent": round(usage, 1),
            "temperature_c": round(temp_raw / 1000.0, 1) if temp_raw else None,
            "memory_total_bytes": _read_int(device / "mem_info_vram_total"),
            "memory_used_bytes": _read_int(device / "mem_info_vram_used"),
        }
    return None


class OptionalCollector:
    """Caches the "this host has no GPU" answer so we do not re-probe every sample."""

    def __init__(self) -> None:
        self._gpu_absent = False

    def collect(self) -> dict[str, Any]:
        gpu: dict[str, Any] | None = None
        if not self._gpu_absent:
            gpu = collect_sysfs_gpu() or collect_nvidia_gpu()
            if gpu is None:
                self._gpu_absent = True
        return {"gpu": gpu, "battery": collect_battery()}
