"""Stable, privacy-preserving device identity.

The ID must survive reboots, IP changes and hostname changes, because the panel keys
its device list on it. It must *not* leak ``/etc/machine-id``: that value is used by
other software as a semi-secret host identifier, so it is salted and hashed here and
only the first 12 hex characters are published.
"""

from __future__ import annotations

import hashlib
import socket
import uuid
from pathlib import Path

# Domain separation: hashing the machine ID with a fixed application salt means the
# published value cannot be correlated with IDs published by unrelated software that
# hashes the same input.
_ID_SALT = b"fleetpanel.device-id.v1"
_ID_LENGTH = 12

_MACHINE_ID_PATHS = (
    Path("/etc/machine-id"),
    Path("/var/lib/dbus/machine-id"),
)


def read_machine_id(paths: tuple[Path, ...] = _MACHINE_ID_PATHS) -> str | None:
    """Return the raw machine ID, or ``None`` when the host has none.

    The raw value never leaves this module's callers in :func:`derive_device_id`.
    """
    for path in paths:
        try:
            value = path.read_text(encoding="ascii").strip()
        except (OSError, UnicodeDecodeError):
            continue
        if value:
            return value
    return None


def derive_device_id(
    machine_id: str | None = None,
    *,
    fallback_seed: str | None = None,
) -> str:
    """Hash the machine ID into a short, stable, non-reversible device ID.

    ``fallback_seed`` is used when no machine ID exists (some containers, some
    minimal images). It defaults to the primary MAC address, which is stable for
    the lifetime of the hardware.
    """
    seed = machine_id if machine_id is not None else read_machine_id()
    if not seed:
        seed = fallback_seed if fallback_seed is not None else _mac_seed()
    digest = hashlib.sha256(_ID_SALT + seed.encode("utf-8")).hexdigest()
    return digest[:_ID_LENGTH]


def _mac_seed() -> str:
    node = uuid.getnode()
    return f"mac:{node:012x}"


def default_display_name() -> str:
    """Hostname without the domain part, used when no name is configured."""
    return socket.gethostname().split(".")[0] or "fleetpanel"


def resolve_identity(configured_id: str, configured_name: str) -> tuple[str, str]:
    """Apply configuration overrides on top of the derived identity."""
    device_id = configured_id.strip().lower() if configured_id else derive_device_id()
    name = configured_name.strip() if configured_name else default_display_name()
    return device_id, name[:64]
