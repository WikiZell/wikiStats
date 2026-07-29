"""CPU, memory, storage, network and host collectors, driven by mocked OS data."""

from __future__ import annotations

from pathlib import Path
from typing import Any, NamedTuple

import pytest

from fleetpanel_agent.collectors import cpu as cpu_mod
from fleetpanel_agent.collectors import host as host_mod
from fleetpanel_agent.collectors import memory as mem_mod
from fleetpanel_agent.collectors import network as net_mod
from fleetpanel_agent.collectors.cpu import CpuCollector
from fleetpanel_agent.collectors.host import (
    HostCollector,
    parse_device_tree_model,
    parse_os_release,
    read_hardware_model,
)
from fleetpanel_agent.collectors.memory import MemoryCollector
from fleetpanel_agent.collectors.network import (
    NetworkCollector,
    compute_rate,
    is_physical,
    parse_proc_net_route,
    sum_counters,
)
from fleetpanel_agent.collectors.optional import parse_nvidia_smi
from fleetpanel_agent.collectors.storage import (
    Partition,
    Usage,
    aggregate,
    build_mounts,
    select_partitions,
)
from fleetpanel_agent.collectors.temperature import TemperatureCollector


class FakeVM(NamedTuple):
    total: int
    available: int
    used: int
    free: int


class FakeSwap(NamedTuple):
    total: int
    used: int
    free: int


class FakeFreq(NamedTuple):
    current: float


class FakeNic(NamedTuple):
    bytes_recv: int
    bytes_sent: int


# ============================================================================ CPU


def _stub_cpu(monkeypatch: pytest.MonkeyPatch, **overrides: Any) -> None:
    monkeypatch.setattr(cpu_mod.psutil, "cpu_percent",
                        lambda interval=None, percpu=False:
                        overrides.get("per_core", [14.0, 22.1, 13.6, 19.8]) if percpu
                        else overrides.get("usage", 17.44))
    monkeypatch.setattr(cpu_mod.psutil, "cpu_count",
                        lambda logical=True: (4 if logical else 4))
    monkeypatch.setattr(cpu_mod.psutil, "cpu_freq", lambda: FakeFreq(1800.0))
    # raising=False: os.getloadavg does not exist on Windows, where CI also runs.
    monkeypatch.setattr(cpu_mod.os, "getloadavg", lambda: (0.41, 0.30, 0.27), raising=False)


def test_cpu_collector_reports_all_fields(monkeypatch: pytest.MonkeyPatch) -> None:
    _stub_cpu(monkeypatch)
    collector = CpuCollector(TemperatureCollector(enabled=False))
    result = collector.collect()
    assert result["usage_percent"] == 17.4
    assert result["per_core_percent"] == [14.0, 22.1, 13.6, 19.8]
    assert result["physical_cores"] == 4
    assert result["logical_cores"] == 4
    assert result["frequency_mhz"] == 1800.0
    assert (result["load_1"], result["load_5"], result["load_15"]) == (0.41, 0.3, 0.27)
    assert result["temperature_c"] is None
    assert result["temperatures"] is None


def test_cpu_frequency_null_when_unavailable(monkeypatch: pytest.MonkeyPatch) -> None:
    """Containers and many ARM boards have no cpufreq; that must not lose the sample."""
    _stub_cpu(monkeypatch)
    monkeypatch.setattr(cpu_mod.psutil, "cpu_freq",
                        lambda: (_ for _ in ()).throw(NotImplementedError()))
    result = CpuCollector(TemperatureCollector(enabled=False)).collect()
    assert result["frequency_mhz"] is None
    assert result["usage_percent"] == 17.4  # everything else survives


def test_cpu_load_null_without_getloadavg(monkeypatch: pytest.MonkeyPatch) -> None:
    _stub_cpu(monkeypatch)
    monkeypatch.setattr(cpu_mod.psutil, "cpu_freq", lambda: FakeFreq(1800.0))
    monkeypatch.setattr(cpu_mod.os, "getloadavg",
                        lambda: (_ for _ in ()).throw(OSError("no loadavg")), raising=False)
    result = CpuCollector(TemperatureCollector(enabled=False)).collect()
    assert result["load_1"] is None


def test_cpu_physical_cores_falls_back_to_logical(monkeypatch: pytest.MonkeyPatch) -> None:
    _stub_cpu(monkeypatch)
    monkeypatch.setattr(cpu_mod.psutil, "cpu_count", lambda logical=True: 8 if logical else None)
    result = CpuCollector(TemperatureCollector(enabled=False)).collect()
    assert result["physical_cores"] == 8
    assert result["logical_cores"] == 8


def test_cpu_usage_null_when_psutil_fails(monkeypatch: pytest.MonkeyPatch) -> None:
    def boom(interval: Any = None, percpu: bool = False) -> Any:
        raise OSError("/proc/stat unreadable")

    monkeypatch.setattr(cpu_mod.psutil, "cpu_percent", boom)
    monkeypatch.setattr(cpu_mod.psutil, "cpu_count", lambda logical=True: 2)
    monkeypatch.setattr(cpu_mod.psutil, "cpu_freq", lambda: None)
    result = CpuCollector(TemperatureCollector(enabled=False)).collect()
    assert result["usage_percent"] is None
    assert result["per_core_percent"] is None
    assert result["logical_cores"] == 2


# ========================================================================= memory


def test_memory_collector(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(
        mem_mod.psutil, "virtual_memory",
        lambda: FakeVM(8589934592, 5368709120, 3221225472, 4294967296),
    )
    monkeypatch.setattr(
        mem_mod.psutil, "swap_memory", lambda: FakeSwap(1073741824, 134217728, 939524096)
    )
    result = MemoryCollector().collect()
    assert result["total_bytes"] == 8589934592
    assert result["available_bytes"] == 5368709120
    assert result["used_bytes"] == 3221225472
    assert result["free_bytes"] == 4294967296
    # 8 GiB total, 5 GiB available -> 37.5% in use.
    assert result["usage_percent"] == 37.5
    assert result["swap_usage_percent"] == 12.5


def test_memory_usage_uses_available_not_used(monkeypatch: pytest.MonkeyPatch) -> None:
    """A machine with 6 GiB of page cache is not 90% full."""
    monkeypatch.setattr(
        mem_mod.psutil, "virtual_memory",
        lambda: FakeVM(total=8_000_000_000, available=6_000_000_000,
                       used=7_200_000_000, free=800_000_000),
    )
    monkeypatch.setattr(mem_mod.psutil, "swap_memory", lambda: FakeSwap(0, 0, 0))
    result = MemoryCollector().collect()
    assert result["usage_percent"] == 25.0


def test_memory_swapless_host(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(
        mem_mod.psutil, "virtual_memory", lambda: FakeVM(4_000_000_000, 2_000_000_000,
                                                         2_000_000_000, 1_500_000_000)
    )
    monkeypatch.setattr(mem_mod.psutil, "swap_memory", lambda: FakeSwap(0, 0, 0))
    result = MemoryCollector().collect()
    assert result["swap_total_bytes"] == 0
    assert result["swap_usage_percent"] == 0.0


def test_memory_all_null_when_psutil_fails(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(mem_mod.psutil, "virtual_memory",
                        lambda: (_ for _ in ()).throw(OSError("boom")))
    result = MemoryCollector().collect()
    assert all(value is None for value in result.values())


# ======================================================================== storage

REAL_MOUNTS = [
    Partition("/dev/mmcblk0p2", "/", "ext4"),
    Partition("/dev/mmcblk0p1", "/boot/firmware", "vfat"),
    Partition("proc", "/proc", "proc"),
    Partition("sysfs", "/sys", "sysfs"),
    Partition("tmpfs", "/run", "tmpfs"),
    Partition("tmpfs", "/dev/shm", "tmpfs"),
    Partition("/dev/loop0", "/snap/core22/1122", "squashfs"),
    Partition("overlay", "/var/lib/docker/overlay2/abc/merged", "overlay"),
    Partition("/dev/sda1", "/mnt/data", "ext4"),
    Partition("/dev/mmcblk0p2", "/mnt/bind-of-root", "ext4"),
    Partition("nas:/export", "/mnt/nas", "nfs4"),
]

USAGES = {
    "/": Usage(62_538_170_368, 27_692_531_712, 34_845_638_656, 44.3),
    "/boot/firmware": Usage(536_870_912, 67_108_864, 469_762_048, 12.5),
    "/mnt/data": Usage(1_000_204_886_016, 400_000_000_000, 600_204_886_016, 40.0),
    "/mnt/bind-of-root": Usage(62_538_170_368, 27_692_531_712, 34_845_638_656, 44.3),
    "/mnt/nas": Usage(4_000_000_000_000, 1_000_000_000_000, 3_000_000_000_000, 25.0),
}


def _usage(mountpoint: str) -> Usage:
    try:
        return USAGES[mountpoint]
    except KeyError:
        raise OSError(f"no usage for {mountpoint}") from None


def test_pseudo_filesystems_are_dropped() -> None:
    chosen = {p.mountpoint for p in select_partitions(REAL_MOUNTS)}
    assert "/proc" not in chosen
    assert "/sys" not in chosen
    assert "/run" not in chosen
    assert "/dev/shm" not in chosen
    assert not any(m.startswith("/snap") for m in chosen)
    assert not any(m.startswith("/var/lib/docker") for m in chosen)
    assert "/" in chosen
    assert "/mnt/data" in chosen


def test_duplicate_backing_device_is_counted_once() -> None:
    mounts = build_mounts(REAL_MOUNTS, _usage, min_total_bytes=64 * 1024 * 1024)
    mountpoints = [m["mountpoint"] for m in mounts]
    assert "/" in mountpoints
    assert "/mnt/bind-of-root" not in mountpoints, "bind mount double-counted the root device"


def test_small_filesystems_are_skipped_but_root_is_never_skipped() -> None:
    mounts = build_mounts(REAL_MOUNTS, _usage, min_total_bytes=1024**3)
    mountpoints = [m["mountpoint"] for m in mounts]
    assert "/boot/firmware" not in mountpoints
    assert "/" in mountpoints


def test_network_shares_are_included_once() -> None:
    mounts = build_mounts(REAL_MOUNTS, _usage)
    nas = [m for m in mounts if m["mountpoint"] == "/mnt/nas"]
    assert len(nas) == 1
    assert nas[0]["filesystem"] == "nfs4"


def test_include_list_restricts_to_named_mounts() -> None:
    """include = ["/"] means the root filesystem, not "every absolute path"."""
    mounts = build_mounts(REAL_MOUNTS, _usage, include=["/"])
    assert [m["mountpoint"] for m in mounts] == ["/"]


def test_include_list_covers_subtrees() -> None:
    mounts = build_mounts(REAL_MOUNTS, _usage, include=["/mnt"])
    assert {m["mountpoint"] for m in mounts} == {"/mnt/data", "/mnt/nas", "/mnt/bind-of-root"}


def test_exclude_list_removes_mounts() -> None:
    mounts = build_mounts(REAL_MOUNTS, _usage, exclude=["/mnt"])
    assert all(not m["mountpoint"].startswith("/mnt") for m in mounts)


def test_unreadable_mount_is_skipped_not_fatal() -> None:
    partitions = [*REAL_MOUNTS, Partition("/dev/sdz1", "/mnt/dead", "ext4")]
    mounts = build_mounts(partitions, _usage)
    assert all(m["mountpoint"] != "/mnt/dead" for m in mounts)
    assert any(m["mountpoint"] == "/" for m in mounts)


def test_aggregate_sums_deduplicated_mounts() -> None:
    mounts = build_mounts(REAL_MOUNTS, _usage, min_total_bytes=1024**3)
    result = aggregate(mounts)
    expected_total = 62_538_170_368 + 1_000_204_886_016 + 4_000_000_000_000
    assert result["total_bytes"] == expected_total
    assert result["used_bytes"] == 27_692_531_712 + 400_000_000_000 + 1_000_000_000_000
    assert 0 < result["usage_percent"] < 100


def test_aggregate_of_nothing_is_all_null() -> None:
    result = aggregate([])
    assert result["total_bytes"] is None
    assert result["mounts"] is None


def test_include_pseudo_keeps_tmpfs() -> None:
    usages = {**USAGES, "/run": Usage(400_000_000, 1_000_000, 399_000_000, 0.25)}
    mounts = build_mounts(
        REAL_MOUNTS, lambda mp: usages.get(mp) or _usage(mp), include_pseudo=True
    )
    assert any(m["mountpoint"] == "/run" for m in mounts)


# ======================================================================== network


def test_compute_rate_first_sample_is_none() -> None:
    assert compute_rate(None, 1000, 2.0) is None


def test_compute_rate_normal() -> None:
    assert compute_rate(1_000_000, 1_049_126, 2.0) == 24563.0


def test_compute_rate_counter_reset_returns_zero() -> None:
    """An interface reset or 32-bit wrap must not produce a negative or huge rate."""
    assert compute_rate(4_294_000_000, 12_345, 2.0) == 0.0


def test_compute_rate_zero_elapsed() -> None:
    assert compute_rate(1000, 2000, 0.0) is None


@pytest.mark.parametrize(
    ("name", "expected"),
    [
        ("eth0", True), ("wlan0", True), ("enp3s0", True), ("end0", True),
        ("lo", False), ("docker0", False), ("br-1a2b3c", False), ("veth9f1", False),
        ("virbr0", False), ("tun0", False), ("wg0", False), ("zt5u4", False),
    ],
)
def test_physical_interface_classification(name: str, expected: bool) -> None:
    assert is_physical(name) is expected


def test_counter_sum_excludes_virtual_interfaces() -> None:
    per_nic = {
        "eth0": FakeNic(1_000, 500),
        "wlan0": FakeNic(2_000, 700),
        "lo": FakeNic(10**9, 10**9),
        "docker0": FakeNic(10**8, 10**8),
        "veth123": FakeNic(10**7, 10**7),
    }
    assert sum_counters(per_nic) == (3_000, 1_200)


PROC_NET_ROUTE = "\n".join(
    [
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT",
        "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0",
        "eth0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0",
        "eth0\t0001A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0",
        "",
    ]
)


def test_default_route_picks_lowest_metric() -> None:
    assert parse_proc_net_route(PROC_NET_ROUTE.splitlines()) == "eth0"


def test_default_route_absent() -> None:
    lines = PROC_NET_ROUTE.splitlines()[:1] + [PROC_NET_ROUTE.splitlines()[3]]
    assert parse_proc_net_route(lines) is None


def test_default_route_ignores_malformed_lines() -> None:
    good = "eth0\t00000000\t0101A8C0\t0003\t0\t0\t50\t0\t0\t0\t0"
    assert parse_proc_net_route(["Iface\tDestination", "garbage", good]) == "eth0"


def test_network_collector_rates_across_two_samples(monkeypatch: pytest.MonkeyPatch) -> None:
    state = {"rx": 1_000_000, "tx": 500_000, "t": 100.0}
    monkeypatch.setattr(net_mod.psutil, "net_io_counters",
                        lambda pernic=False: {"eth0": FakeNic(state["rx"], state["tx"])})
    monkeypatch.setattr(net_mod, "ipv4_addresses", lambda: ["192.168.1.50"])
    monkeypatch.setattr(net_mod, "read_default_interface", lambda *a, **k: "eth0")
    monkeypatch.setattr(net_mod.time, "monotonic", lambda: state["t"])

    collector = NetworkCollector()
    first = collector.collect()
    assert first["rx_bytes_per_second"] == 0.0
    assert first["rx_bytes_total"] == 1_000_000

    state["rx"] += 49_126
    state["tx"] += 8_623
    state["t"] += 2.0
    second = collector.collect()
    assert second["rx_bytes_per_second"] == 24563.0
    assert second["tx_bytes_per_second"] == 4311.5
    assert second["primary_interface"] == "eth0"
    assert second["ip_addresses"] == ["192.168.1.50"]


def test_network_collector_survives_counter_failure(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(net_mod.psutil, "net_io_counters",
                        lambda pernic=False: (_ for _ in ()).throw(OSError("boom")))
    monkeypatch.setattr(net_mod, "ipv4_addresses", lambda: [])
    result = NetworkCollector().collect()
    assert result["rx_bytes_total"] is None
    assert result["ip_addresses"] is None


# =========================================================================== host


def test_parse_os_release_debian() -> None:
    text = """PRETTY_NAME="Debian GNU/Linux 12 (bookworm)"
NAME="Debian GNU/Linux"
VERSION_ID="12"
VERSION="12 (bookworm)"
ID=debian
# a comment

HOME_URL="https://www.debian.org/"
"""
    parsed = parse_os_release(text)
    assert parsed["PRETTY_NAME"] == "Debian GNU/Linux 12 (bookworm)"
    assert parsed["VERSION_ID"] == "12"
    assert parsed["ID"] == "debian"


def test_parse_os_release_unquoted_values() -> None:
    parsed = parse_os_release("ID=ubuntu\nVERSION_ID=24.04\n")
    assert parsed == {"ID": "ubuntu", "VERSION_ID": "24.04"}


def test_device_tree_model_is_nul_terminated() -> None:
    raw = b"Raspberry Pi 4 Model B Rev 1.5\x00"
    assert parse_device_tree_model(raw) == "Raspberry Pi 4 Model B Rev 1.5"


def test_device_tree_model_with_padding() -> None:
    raw = b"Raspberry Pi 5 Model B Rev 1.0\x00\x00\x00\x00"
    assert parse_device_tree_model(raw) == "Raspberry Pi 5 Model B Rev 1.0"


def test_hardware_model_prefers_device_tree(tmp_path: Path) -> None:
    dt = tmp_path / "model"
    dt.write_bytes(b"Raspberry Pi 4 Model B Rev 1.5\x00")
    dmi = tmp_path / "dmi"
    dmi.mkdir()
    (dmi / "sys_vendor").write_text("Should Not Win")
    (dmi / "product_name").write_text("Nope")
    assert read_hardware_model(dt, dmi) == "Raspberry Pi 4 Model B Rev 1.5"


def test_hardware_model_falls_back_to_dmi(tmp_path: Path) -> None:
    dmi = tmp_path / "dmi"
    dmi.mkdir()
    (dmi / "sys_vendor").write_text("Dell Inc.\n")
    (dmi / "product_name").write_text("OptiPlex 7070\n")
    assert read_hardware_model(tmp_path / "absent", dmi) == "Dell Inc. OptiPlex 7070"


def test_hardware_model_null_when_nothing_known(tmp_path: Path) -> None:
    assert read_hardware_model(tmp_path / "absent", tmp_path / "also-absent") is None


def test_host_status_reports_degraded(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(host_mod.psutil, "boot_time", lambda: 1_769_000_000.0)
    monkeypatch.setattr(host_mod.psutil, "pids", lambda: list(range(143)))
    monkeypatch.setattr(host_mod.psutil, "users", lambda: [])
    collector = HostCollector("abc123", "Test")
    assert collector.status()["state"] == "online"
    assert collector.status(degraded=True)["state"] == "degraded"


def test_host_status_counts_distinct_users(monkeypatch: pytest.MonkeyPatch) -> None:
    class U(NamedTuple):
        name: str

    monkeypatch.setattr(host_mod.psutil, "boot_time", lambda: 1_769_000_000.0)
    monkeypatch.setattr(host_mod.psutil, "pids", lambda: [1, 2])
    monkeypatch.setattr(host_mod.psutil, "users", lambda: [U("pi"), U("pi"), U("root")])
    status = HostCollector("abc123", "Test").status()
    assert status["logged_in_users"] == 2


def test_host_status_survives_sensor_failure(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(host_mod.psutil, "boot_time",
                        lambda: (_ for _ in ()).throw(OSError("boom")))
    monkeypatch.setattr(host_mod.psutil, "pids", lambda: (_ for _ in ()).throw(OSError("boom")))
    monkeypatch.setattr(host_mod.psutil, "users", lambda: (_ for _ in ()).throw(OSError("boom")))
    status = HostCollector("abc123", "Test").status()
    assert status["uptime_seconds"] is None
    assert status["boot_time"] is None
    assert status["process_count"] is None
    assert status["logged_in_users"] is None


# ======================================================================= optional


def test_nvidia_smi_parsing() -> None:
    parsed = parse_nvidia_smi("NVIDIA GeForce RTX 3060, 34, 51, 12288, 2048\n")
    assert parsed is not None
    assert parsed["name"] == "NVIDIA GeForce RTX 3060"
    assert parsed["usage_percent"] == 34.0
    assert parsed["temperature_c"] == 51.0
    assert parsed["memory_total_bytes"] == 12288 * 1024 * 1024


@pytest.mark.parametrize("text", ["", "no,gpu", "a, b, c, d"])
def test_nvidia_smi_rejects_short_output(text: str) -> None:
    assert parse_nvidia_smi(text) is None
