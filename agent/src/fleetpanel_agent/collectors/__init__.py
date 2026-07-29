"""Subsystem collectors.

Every collector returns a plain ``dict`` matching one branch of
``fleetpanel.telemetry.v1``, with ``None`` for values the host cannot supply. A
collector must never raise for a missing sensor: the sampler catches exceptions as a
last resort, but a collector that swallows its own partial failures produces far more
useful telemetry than one that gives up on the whole subsystem.
"""

from __future__ import annotations

from .cpu import CpuCollector
from .host import HostCollector
from .memory import MemoryCollector
from .network import NetworkCollector
from .optional import OptionalCollector
from .storage import StorageCollector
from .temperature import TemperatureCollector

__all__ = [
    "CpuCollector",
    "HostCollector",
    "MemoryCollector",
    "NetworkCollector",
    "OptionalCollector",
    "StorageCollector",
    "TemperatureCollector",
]
