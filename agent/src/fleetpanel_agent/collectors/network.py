"""Network counters, rates, primary interface and IPv4 addresses.

Totals aggregate every physical, non-loopback interface. Virtual interfaces
(``docker0``, ``veth*``, bridges, VPN tunnels) are excluded because their traffic is
usually the same packets counted a second time, which would double every rate on a
container host.

Rates are computed from the delta between consecutive samples using a monotonic
clock, so an NTP step does not produce a nonsense spike. Counter wrap or an interface
disappearing yields ``0.0`` for that interval rather than a negative or astronomical
value.
"""

from __future__ import annotations

import logging
import socket
import time
from collections.abc import Mapping
from pathlib import Path
from typing import Any, NamedTuple

import psutil

from ..logging_setup import get_logger, log_throttled

_log = get_logger("network")

_PROC_NET_ROUTE = Path("/proc/net/route")

VIRTUAL_PREFIXES: tuple[str, ...] = (
    "lo",
    "docker",
    "br-",
    "veth",
    "virbr",
    "vmnet",
    "vboxnet",
    "tun",
    "tap",
    "wg",
    "zt",
    "cni",
    "flannel",
    "kube",
    "ifb",
    "dummy",
)


class Counters(NamedTuple):
    rx: int
    tx: int


def is_physical(name: str) -> bool:
    """True for interfaces whose byte counters should feed the aggregate."""
    lowered = name.lower()
    if lowered == "lo" or lowered.startswith("lo:"):
        return False
    return not any(lowered.startswith(prefix) for prefix in VIRTUAL_PREFIXES)


def sum_counters(per_nic: Mapping[str, Any]) -> Counters:
    rx = tx = 0
    for name, counters in per_nic.items():
        if not is_physical(name):
            continue
        rx += int(getattr(counters, "bytes_recv", 0))
        tx += int(getattr(counters, "bytes_sent", 0))
    return Counters(rx, tx)


def compute_rate(previous: int | None, current: int, elapsed: float) -> float | None:
    """Bytes per second between two cumulative counter readings.

    Returns ``None`` before a baseline exists, and ``0.0`` when the counter went
    backwards (interface reset, 32-bit wrap, NIC replaced).
    """
    if previous is None or elapsed <= 0:
        return None
    delta = current - previous
    if delta < 0:
        return 0.0
    return round(delta / elapsed, 1)


def read_default_interface(route_file: Path = _PROC_NET_ROUTE) -> str | None:
    """Interface holding the IPv4 default route, parsed from ``/proc/net/route``."""
    try:
        lines = route_file.read_text(encoding="ascii").splitlines()
    except OSError:
        return None
    return parse_proc_net_route(lines)


def parse_proc_net_route(lines: list[str]) -> str | None:
    """Pure parser for ``/proc/net/route``. Exposed for tests.

    Picks the default route (destination ``00000000``) with the lowest metric.
    """
    best_iface: str | None = None
    best_metric = 1 << 31
    for line in lines[1:]:
        fields = line.split()
        if len(fields) < 8:
            continue
        iface, destination, _gateway, flags = fields[0], fields[1], fields[2], fields[3]
        if destination != "00000000":
            continue
        try:
            if not int(flags, 16) & 0x2:  # RTF_GATEWAY
                continue
            metric = int(fields[6])
        except ValueError:
            continue
        if metric < best_metric:
            best_metric = metric
            best_iface = iface
    return best_iface


def primary_interface_via_socket() -> str | None:
    """Fallback: find the interface holding the address the kernel would source from.

    Opening a UDP socket to a routable address sends no packets; it only makes the
    kernel run its route lookup, which works on every platform including Windows.
    """
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(0.2)
            sock.connect(("192.0.2.1", 9))  # TEST-NET-1, guaranteed unrouted
            local_ip = sock.getsockname()[0]
    except OSError:
        return None
    try:
        for name, addrs in psutil.net_if_addrs().items():
            for addr in addrs:
                if addr.family == socket.AF_INET and addr.address == local_ip:
                    return name
    except Exception:  # noqa: BLE001
        return None
    return None


def ipv4_addresses() -> list[str]:
    out: list[str] = []
    try:
        for name, addrs in psutil.net_if_addrs().items():
            if name.lower() == "lo":
                continue
            for addr in addrs:
                if addr.family != socket.AF_INET:
                    continue
                if not addr.address or addr.address.startswith("127."):
                    continue
                if addr.address not in out:
                    out.append(addr.address)
    except Exception:  # noqa: BLE001
        log_throttled(_log, logging.WARNING, "net.addrs", "net_if_addrs failed")
    return out


class NetworkCollector:
    def __init__(self) -> None:
        self._prev: Counters | None = None
        self._prev_time: float | None = None

    def collect(self) -> dict[str, Any]:
        now = time.monotonic()
        try:
            per_nic = psutil.net_io_counters(pernic=True)
        except Exception:  # noqa: BLE001
            log_throttled(_log, logging.WARNING, "net.counters", "net_io_counters failed")
            return {
                "primary_interface": None,
                "ip_addresses": ipv4_addresses() or None,
                "rx_bytes_total": None,
                "tx_bytes_total": None,
                "rx_bytes_per_second": None,
                "tx_bytes_per_second": None,
            }

        current = sum_counters(per_nic)
        elapsed = (now - self._prev_time) if self._prev_time is not None else 0.0
        rx_rate = compute_rate(self._prev.rx if self._prev else None, current.rx, elapsed)
        tx_rate = compute_rate(self._prev.tx if self._prev else None, current.tx, elapsed)
        self._prev = current
        self._prev_time = now

        primary = read_default_interface() or primary_interface_via_socket()

        return {
            "primary_interface": primary,
            "ip_addresses": ipv4_addresses() or None,
            "rx_bytes_total": current.rx,
            "tx_bytes_total": current.tx,
            # Before the second sample there is no measurable rate; report 0.0 rather
            # than null so the panel's bar graphs do not have to special-case startup.
            "rx_bytes_per_second": rx_rate if rx_rate is not None else 0.0,
            "tx_bytes_per_second": tx_rate if tx_rate is not None else 0.0,
        }
