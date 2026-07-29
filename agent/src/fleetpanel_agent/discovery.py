"""mDNS / DNS-SD advertisement of ``_fleetpanel._tcp.local.``

The Python ``zeroconf`` library is used rather than shelling out to ``avahi-publish``
so discovery works on images that do not ship Avahi (many Docker bases, some Pi Lite
installs) and so the agent controls its own TXT records.

TXT keys are ASCII and short because DNS-SD TXT records are size limited and some
resolvers truncate long records:

``id`` stable device ID - ``name`` display name - ``schema`` protocol major version
- ``path`` telemetry URL path - ``transport`` ``http`` - ``auth`` ``none``/``token``
- ``platform`` OS family.
"""

from __future__ import annotations

import contextlib
import socket
from typing import Any

from zeroconf import IPVersion, ServiceInfo, Zeroconf

from . import MDNS_SERVICE_TYPE
from .config import AgentConfig
from .logging_setup import get_logger

_log = get_logger("discovery")

# DNS-SD instance names must not contain a dot; it would be read as a label separator.
_UNSAFE_INSTANCE_CHARS = str.maketrans({".": "-", "/": "-"})


def build_txt_properties(
    device_id: str, display_name: str, auth_mode: str, platform: str = "linux"
) -> dict[bytes, bytes]:
    return {
        b"id": device_id.encode("ascii", "ignore"),
        b"name": display_name.encode("utf-8")[:63],
        b"schema": b"1",
        b"path": b"/api/v1/telemetry",
        b"transport": b"http",
        b"auth": b"none" if auth_mode == "none" else b"token",
        b"platform": platform.encode("ascii", "ignore"),
    }


def instance_name(display_name: str, device_id: str) -> str:
    """``<name>-<id>._fleetpanel._tcp.local.`` - the ID suffix prevents collisions
    when two machines share a display name."""
    safe = display_name.translate(_UNSAFE_INSTANCE_CHARS).strip() or "fleetpanel"
    return f"{safe[:40]}-{device_id[:8]}.{MDNS_SERVICE_TYPE}"


def local_addresses() -> list[bytes]:
    """Packed IPv4 addresses for the ServiceInfo record."""
    from .collectors.network import ipv4_addresses

    packed: list[bytes] = []
    for address in ipv4_addresses():
        try:
            packed.append(socket.inet_aton(address))
        except OSError:
            continue
    return packed


class DiscoveryAdvertiser:
    """Registers, and cleanly unregisters, the mDNS service."""

    def __init__(self, config: AgentConfig, device_id: str, display_name: str) -> None:
        self.config = config
        self.device_id = device_id
        self.display_name = config.discovery.service_name or display_name
        self._zeroconf: Zeroconf | None = None
        self._info: ServiceInfo | None = None

    def build_info(self) -> ServiceInfo:
        addresses = local_addresses()
        return ServiceInfo(
            MDNS_SERVICE_TYPE,
            instance_name(self.display_name, self.device_id),
            addresses=addresses,
            port=self.config.http.port,
            properties=build_txt_properties(
                self.device_id, self.display_name, self.config.http.auth_mode
            ),
            server=f"{socket.gethostname().split('.')[0]}-fleetpanel.local.",
        )

    def start(self) -> None:
        if not self.config.discovery.enabled:
            _log.info("mDNS discovery disabled by configuration")
            return
        if self._zeroconf is not None:
            return
        try:
            self._zeroconf = Zeroconf(ip_version=IPVersion.V4Only)
            self._info = self.build_info()
            self._zeroconf.register_service(self._info, allow_name_change=True)
        except Exception:  # noqa: BLE001 - discovery is a nicety, never fatal
            _log.warning("mDNS registration failed; continuing without discovery", exc_info=True)
            self.stop()
            return
        _log.info(
            "mDNS service registered",
            extra={"fields": {"name": self._info.name if self._info else "?",
                              "port": self.config.http.port}},
        )

    def stop(self) -> None:
        zeroconf, info = self._zeroconf, self._info
        self._zeroconf, self._info = None, None
        if zeroconf is None:
            return
        try:
            if info is not None:
                zeroconf.unregister_service(info)
        except Exception:  # noqa: BLE001 - shutdown must not raise
            _log.debug("mDNS unregister failed", exc_info=True)
        finally:
            with contextlib.suppress(Exception):
                zeroconf.close()

    def describe(self) -> dict[str, Any]:
        return {
            "enabled": self.config.discovery.enabled,
            "service_type": MDNS_SERVICE_TYPE,
            "instance": instance_name(self.display_name, self.device_id),
            "port": self.config.http.port,
            "txt": {
                k.decode(): v.decode(errors="replace")
                for k, v in build_txt_properties(
                    self.device_id, self.display_name, self.config.http.auth_mode
                ).items()
            },
        }
