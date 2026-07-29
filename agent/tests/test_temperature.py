"""Temperature source layering, parsing and primary-sensor selection."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from fleetpanel_agent.collectors import temperature as temp_mod
from fleetpanel_agent.collectors.temperature import (
    DEFAULT_PREFERRED_LABELS,
    TemperatureCollector,
    parse_sensors_json,
    parse_vcgencmd_output,
    read_sysfs_thermal,
    select_primary,
)


def _r(
    label: str, value: float, high: float | None = None, crit: float | None = None
) -> dict[str, Any]:
    return {"label": label, "temperature_c": value, "high_c": high, "critical_c": crit}


# ------------------------------------------------------------------- selection


def test_primary_prefers_coretemp_over_acpitz() -> None:
    readings = [_r("acpitz", 72.0), _r("coretemp Package id 0", 44.0)]
    assert select_primary(readings) == 44.0


def test_primary_prefers_k10temp_on_amd() -> None:
    readings = [_r("acpitz", 30.0), _r("k10temp Tctl", 51.5), _r("nvme Composite", 40.0)]
    assert select_primary(readings) == 51.5


def test_primary_uses_cpu_thermal_on_raspberry_pi() -> None:
    readings = [_r("rp1_adc", 25.0), _r("cpu_thermal", 48.2)]
    assert select_primary(readings) == 48.2


def test_hottest_wins_within_the_same_priority() -> None:
    """coretemp reports one entry per core; the package maximum is what throttles."""
    readings = [
        _r("coretemp Core 0", 41.0),
        _r("coretemp Core 1", 55.0),
        _r("coretemp Core 2", 47.0),
    ]
    assert select_primary(readings) == 55.0


def test_unknown_labels_are_used_only_as_a_last_resort() -> None:
    assert select_primary([_r("mystery_sensor", 61.0)]) == 61.0


def test_no_readings_returns_none() -> None:
    assert select_primary([]) is None


def test_implausible_values_are_ignored() -> None:
    readings = [_r("coretemp", 48000.0), _r("acpitz", 39.0)]
    assert select_primary(readings) == 39.0


def test_custom_priority_list_is_honoured() -> None:
    readings = [_r("coretemp", 40.0), _r("nvme", 62.0)]
    assert select_primary(readings, preferred=["nvme"]) == 62.0
    assert select_primary(readings, preferred=DEFAULT_PREFERRED_LABELS) == 40.0


# --------------------------------------------------------------------- parsing


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        ("temp=48.2'C\n", 48.2),
        ("temp=61.0'C", 61.0),
        ("temp=5'C\n", 5.0),
    ],
)
def test_vcgencmd_parsing(text: str, expected: float) -> None:
    parsed = parse_vcgencmd_output(text)
    assert parsed is not None
    assert parsed["temperature_c"] == expected
    assert "vcgencmd" in parsed["label"]


@pytest.mark.parametrize("text", ["", "VCHI initialization failed", "temp=abc'C", "temp="])
def test_vcgencmd_rejects_garbage(text: str) -> None:
    assert parse_vcgencmd_output(text) is None


def test_sensors_json_parsing() -> None:
    payload = """
    {
      "coretemp-isa-0000": {
        "Adapter": "ISA adapter",
        "Package id 0": {"temp1_input": 45.0, "temp1_max": 80.0, "temp1_crit": 100.0},
        "Core 0": {"temp2_input": 43.0, "temp2_max": 80.0, "temp2_crit": 100.0}
      },
      "nvme-pci-0100": {
        "Composite": {"temp1_input": 38.85, "temp1_crit": 84.85}
      }
    }
    """
    readings = parse_sensors_json(payload)
    labels = {r["label"] for r in readings}
    assert "coretemp-isa-0000 Package id 0" in labels
    assert "nvme-pci-0100 Composite" in labels
    package = next(r for r in readings if r["label"].endswith("Package id 0"))
    assert package["temperature_c"] == 45.0
    assert package["high_c"] == 80.0
    assert package["critical_c"] == 100.0
    # "Adapter" is a string, not a feature object, and must be skipped.
    assert not any("Adapter" in r["label"] for r in readings)


@pytest.mark.parametrize("text", ["", "not json", "[]", "null"])
def test_sensors_json_rejects_garbage(text: str) -> None:
    assert parse_sensors_json(text) == []


def test_sysfs_thermal_reading(tmp_path: Path) -> None:
    for index, (zone_type, millidegrees) in enumerate(
        [("cpu-thermal", "48200"), ("gpu-thermal", "45100")]
    ):
        zone = tmp_path / f"thermal_zone{index}"
        zone.mkdir()
        (zone / "type").write_text(zone_type)
        (zone / "temp").write_text(millidegrees + "\n")
    readings = read_sysfs_thermal(tmp_path)
    assert {r["label"] for r in readings} == {"cpu-thermal", "gpu-thermal"}
    assert readings[0]["temperature_c"] == 48.2


def test_sysfs_thermal_skips_unreadable_zone(tmp_path: Path) -> None:
    good = tmp_path / "thermal_zone0"
    good.mkdir()
    (good / "type").write_text("cpu-thermal")
    (good / "temp").write_text("52000")
    broken = tmp_path / "thermal_zone1"
    broken.mkdir()
    (broken / "type").write_text("broken")
    (broken / "temp").write_text("not-a-number")
    readings = read_sysfs_thermal(tmp_path)
    assert len(readings) == 1
    assert readings[0]["temperature_c"] == 52.0


def test_sysfs_thermal_on_missing_directory(tmp_path: Path) -> None:
    assert read_sysfs_thermal(tmp_path / "nope") == []


# ------------------------------------------------------------------- collector


def test_collector_returns_null_when_no_sensors_exist(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(temp_mod, "read_psutil_sensors", lambda: [])
    monkeypatch.setattr(temp_mod, "read_sysfs_thermal", lambda *a, **k: [])
    monkeypatch.setattr(temp_mod, "read_vcgencmd", lambda *a, **k: None)
    monkeypatch.setattr(temp_mod, "read_sensors_json", lambda *a, **k: [])
    primary, readings = TemperatureCollector().collect()
    assert primary is None
    assert readings == []


def test_collector_falls_back_from_psutil_to_sysfs(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(temp_mod, "read_psutil_sensors", lambda: [])
    monkeypatch.setattr(
        temp_mod, "read_sysfs_thermal", lambda *a, **k: [_r("cpu_thermal", 55.5)]
    )
    monkeypatch.setattr(temp_mod, "read_vcgencmd", lambda *a, **k: None)
    primary, readings = TemperatureCollector().collect()
    assert primary == 55.5
    assert len(readings) == 1


def test_collector_adds_vcgencmd_reading_on_a_pi(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(temp_mod, "read_psutil_sensors", lambda: [_r("rp1_adc", 25.0)])
    monkeypatch.setattr(temp_mod, "read_sysfs_thermal", lambda *a, **k: [])
    monkeypatch.setattr(
        temp_mod, "read_vcgencmd", lambda *a, **k: _r("cpu_thermal (vcgencmd)", 49.7)
    )
    primary, readings = TemperatureCollector().collect()
    assert primary == 49.7
    assert len(readings) == 2


def test_collector_disabled_returns_null(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(temp_mod, "read_psutil_sensors", lambda: [_r("coretemp", 40.0)])
    primary, readings = TemperatureCollector(enabled=False).collect()
    assert primary is None
    assert readings == []


def test_sensors_json_source_is_opt_in(monkeypatch: pytest.MonkeyPatch) -> None:
    called: list[str] = []
    monkeypatch.setattr(temp_mod, "read_psutil_sensors", lambda: [_r("coretemp", 40.0)])
    monkeypatch.setattr(temp_mod, "read_vcgencmd", lambda *a, **k: None)

    def spy(*_a: object, **_k: object) -> list[dict[str, Any]]:
        called.append("yes")
        return []

    monkeypatch.setattr(temp_mod, "read_sensors_json", spy)
    TemperatureCollector(use_sensors_json=False).collect()
    assert called == []
    TemperatureCollector(use_sensors_json=True).collect()
    assert called == ["yes"]
