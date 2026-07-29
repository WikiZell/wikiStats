"""Host identity and status: OS, kernel, hardware model, uptime, processes, sessions.

Hardware model detection covers the two families that matter for this project:

* Device tree (``/proc/device-tree/model``) - every Raspberry Pi and most ARM SBCs.
  This is what produces ``"Raspberry Pi 4 Model B Rev 1.5"``.
* DMI (``/sys/class/dmi/id/*``) - x86 desktops, laptops and servers.
"""

from __future__ import annotations

import datetime as dt
import logging
import platform
import socket
import time
from pathlib import Path
from typing import Any

import psutil

from .. import AGENT_VERSION
from ..logging_setup import get_logger, log_throttled

_log = get_logger("host")

_OS_RELEASE = Path("/etc/os-release")
_DEVICE_TREE_MODEL = Path("/proc/device-tree/model")
_DMI_DIR = Path("/sys/class/dmi/id")


def parse_os_release(text: str) -> dict[str, str]:
    """Parse ``/etc/os-release`` key=value lines, stripping quotes."""
    out: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        out[key.strip()] = value
    return out


def read_os_release(path: Path = _OS_RELEASE) -> dict[str, str]:
    try:
        return parse_os_release(path.read_text(encoding="utf-8"))
    except OSError:
        return {}


def parse_device_tree_model(raw: bytes) -> str | None:
    """Device-tree strings are NUL terminated and sometimes NUL padded."""
    text = raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace").strip()
    return text or None


def read_hardware_model(
    device_tree: Path = _DEVICE_TREE_MODEL, dmi_dir: Path = _DMI_DIR
) -> str | None:
    try:
        if device_tree.exists():
            model = parse_device_tree_model(device_tree.read_bytes())
            if model:
                return model
    except OSError:
        pass
    try:
        vendor = (dmi_dir / "sys_vendor").read_text(encoding="utf-8").strip()
    except OSError:
        vendor = ""
    try:
        product = (dmi_dir / "product_name").read_text(encoding="utf-8").strip()
    except OSError:
        product = ""
    combined = " ".join(part for part in (vendor, product) if part).strip()
    return combined or None


def utc_iso(timestamp: float) -> str:
    return dt.datetime.fromtimestamp(timestamp, tz=dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


class HostCollector:
    """Static identity is read once; status is re-read every sample."""

    def __init__(self, device_id: str, display_name: str) -> None:
        self.device_id = device_id
        self.display_name = display_name
        self._static = self._read_static()

    def _read_static(self) -> dict[str, Any]:
        release = read_os_release()
        os_name = release.get("PRETTY_NAME") or release.get("NAME") or platform.system()
        os_version = release.get("VERSION_ID") or release.get("VERSION") or platform.version()
        return {
            "id": self.device_id,
            "name": self.display_name,
            "hostname": socket.gethostname(),
            "platform": self._platform_family(),
            "os_name": os_name or None,
            "os_version": os_version or None,
            "kernel": platform.release() or None,
            "architecture": platform.machine() or None,
            "hardware_model": read_hardware_model(),
            "agent_version": AGENT_VERSION,
        }

    @staticmethod
    def _platform_family() -> str:
        system = platform.system().lower()
        if system == "linux":
            return "linux"
        if system == "windows":
            return "windows"
        if system == "darwin":
            return "macos"
        if "bsd" in system:
            return "freebsd"
        return "other"

    def device(self) -> dict[str, Any]:
        return dict(self._static)

    def status(self, degraded: bool = False) -> dict[str, Any]:
        boot: float | None
        try:
            boot = float(psutil.boot_time())
        except Exception:  # noqa: BLE001
            log_throttled(_log, logging.WARNING, "host.boot", "boot_time unavailable")
            boot = None

        uptime = int(max(time.time() - boot, 0)) if boot else None

        try:
            process_count: int | None = len(psutil.pids())
        except Exception:  # noqa: BLE001
            log_throttled(_log, logging.DEBUG, "host.pids", "pid enumeration failed")
            process_count = None

        try:
            # Distinct usernames, not sessions: three terminals is still one user.
            logged_in: int | None = len({u.name for u in psutil.users()})
        except Exception:  # noqa: BLE001
            log_throttled(_log, logging.DEBUG, "host.users", "user enumeration failed")
            logged_in = None

        return {
            "state": "degraded" if degraded else "online",
            "uptime_seconds": uptime,
            "boot_time": utc_iso(boot) if boot else None,
            "process_count": process_count,
            "logged_in_users": logged_in,
        }
