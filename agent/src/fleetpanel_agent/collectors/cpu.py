"""CPU metrics.

``psutil.cpu_percent(interval=None)`` is deliberately non-blocking: it reports usage
since the previous call. The sampler primes it once at startup and then calls it on a
fixed cadence, so the value is always a real measurement over the sampling interval
and an HTTP request never has to wait a second for one.
"""

from __future__ import annotations

import logging
import os
from typing import Any

import psutil

from ..logging_setup import get_logger, log_throttled
from .temperature import TemperatureCollector

_log = get_logger("cpu")


class CpuCollector:
    def __init__(self, temperature: TemperatureCollector | None = None) -> None:
        self._temperature = temperature or TemperatureCollector()
        self._primed = False

    def prime(self) -> None:
        """Establish the baseline for the delta-based percentages."""
        try:
            psutil.cpu_percent(interval=None)
            psutil.cpu_percent(interval=None, percpu=True)
        except Exception:  # noqa: BLE001 - priming must never abort startup
            log_throttled(_log, logging.WARNING, "cpu.prime", "cpu_percent priming failed")
        self._primed = True

    def collect(self) -> dict[str, Any]:
        if not self._primed:
            self.prime()

        usage: float | None
        try:
            usage = round(float(psutil.cpu_percent(interval=None)), 1)
        except Exception:  # noqa: BLE001
            log_throttled(_log, logging.WARNING, "cpu.usage", "cpu_percent failed")
            usage = None

        per_core: list[float] | None
        try:
            per_core = [round(float(v), 1) for v in psutil.cpu_percent(interval=None, percpu=True)]
        except Exception:  # noqa: BLE001
            log_throttled(_log, logging.WARNING, "cpu.percore", "per-core cpu_percent failed")
            per_core = None

        try:
            logical = psutil.cpu_count(logical=True)
        except Exception:  # noqa: BLE001
            logical = None
        try:
            physical = psutil.cpu_count(logical=False)
        except Exception:  # noqa: BLE001
            physical = None
        # Containers and some ARM boards report None for physical cores.
        if physical is None:
            physical = logical

        frequency_mhz: float | None = None
        try:
            freq = psutil.cpu_freq()
            if freq is not None and freq.current:
                frequency_mhz = round(float(freq.current), 1)
        except Exception:  # noqa: BLE001 - not available in many containers
            log_throttled(_log, logging.DEBUG, "cpu.freq", "cpu_freq unavailable")

        load_1 = load_5 = load_15 = None
        try:
            raw = os.getloadavg() if hasattr(os, "getloadavg") else psutil.getloadavg()
            load_1, load_5, load_15 = (round(float(v), 2) for v in raw)
        except (OSError, AttributeError, ValueError):
            log_throttled(_log, logging.DEBUG, "cpu.load", "load average unavailable")

        primary_temp, temperatures = self._temperature.collect()

        return {
            "usage_percent": usage,
            "per_core_percent": per_core,
            "physical_cores": physical,
            "logical_cores": logical,
            "frequency_mhz": frequency_mhz,
            "load_1": load_1,
            "load_5": load_5,
            "load_15": load_15,
            "temperature_c": primary_temp,
            "temperatures": temperatures or None,
        }
