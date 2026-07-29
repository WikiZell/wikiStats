"""Background sampler.

A single daemon thread owns all measurement. HTTP handlers only read the last
completed sample from an atomically swapped reference, so a request never blocks and
never observes a half-built document.

Failure policy: each subsystem is collected inside its own guard. A subsystem that
raises contributes ``null`` values, drops its capability flag, and flips
``status.state`` to ``degraded`` for that sample - the agent keeps serving everything
else.
"""

from __future__ import annotations

import datetime as dt
import logging
import threading
import time
from collections.abc import Callable, Iterator
from queue import Empty, Full, Queue
from typing import Any, Protocol, runtime_checkable

from . import AGENT_VERSION, META_SCHEMA, TELEMETRY_SCHEMA
from .collectors import (
    CpuCollector,
    HostCollector,
    MemoryCollector,
    NetworkCollector,
    OptionalCollector,
    StorageCollector,
    TemperatureCollector,
)
from .config import AgentConfig
from .logging_setup import get_logger, log_throttled
from .ringbuffer import HistoryBuffer, capacity_for

_log = get_logger("sampler")

# Subscriber queues are small: an SSE client that cannot keep up should lose old
# frames, not grow the agent's heap.
_SUBSCRIBER_QUEUE_SIZE = 4

_NULL_CPU: dict[str, Any] = {
    "usage_percent": None,
    "per_core_percent": None,
    "physical_cores": None,
    "logical_cores": None,
    "frequency_mhz": None,
    "load_1": None,
    "load_5": None,
    "load_15": None,
    "temperature_c": None,
    "temperatures": None,
}
_NULL_MEMORY: dict[str, Any] = {
    "total_bytes": None,
    "available_bytes": None,
    "used_bytes": None,
    "free_bytes": None,
    "usage_percent": None,
    "swap_total_bytes": None,
    "swap_used_bytes": None,
    "swap_free_bytes": None,
    "swap_usage_percent": None,
}
_NULL_STORAGE: dict[str, Any] = {
    "total_bytes": None,
    "used_bytes": None,
    "free_bytes": None,
    "usage_percent": None,
    "mounts": None,
}
_NULL_NETWORK: dict[str, Any] = {
    "primary_interface": None,
    "ip_addresses": None,
    "rx_bytes_total": None,
    "tx_bytes_total": None,
    "rx_bytes_per_second": None,
    "tx_bytes_per_second": None,
}


def utc_now_iso() -> str:
    # dt.timezone.utc rather than dt.UTC, which is 3.11+ only.
    return dt.datetime.now(tz=dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def derive_capabilities(sample: dict[str, Any]) -> list[str]:
    """Capabilities are a function of the data, never a static list.

    A capability is present only when the corresponding value is non-null, which is
    what lets the panel hide a tile instead of drawing ``--``.
    """
    cpu = sample.get("cpu") or {}
    memory = sample.get("memory") or {}
    storage = sample.get("storage") or {}
    network = sample.get("network") or {}
    optional = sample.get("optional") or {}
    status = sample.get("status") or {}

    caps: list[str] = []
    if cpu.get("usage_percent") is not None:
        caps.append("cpu")
    if cpu.get("temperature_c") is not None:
        caps.append("cpu_temperature")
    if cpu.get("frequency_mhz") is not None:
        caps.append("cpu_frequency")
    if cpu.get("load_1") is not None:
        caps.append("load_average")
    if memory.get("total_bytes"):
        caps.append("memory")
    if memory.get("swap_total_bytes"):
        caps.append("swap")
    if storage.get("total_bytes"):
        caps.append("storage")
    if network.get("rx_bytes_total") is not None:
        caps.append("network")
    if optional.get("gpu") is not None:
        caps.append("gpu")
    if optional.get("battery") is not None:
        caps.append("battery")
    if status.get("process_count") is not None:
        caps.append("processes")
    if status.get("logged_in_users") is not None:
        caps.append("sessions")
    return caps


@runtime_checkable
class TelemetrySource(Protocol):
    """What the HTTP API needs from a sample producer.

    Both the real :class:`Sampler` and the development simulator implement it, which
    is why ``create_app`` can serve fabricated fleets without a second web layer.
    """

    def latest(self) -> dict[str, Any] | None: ...
    def latest_age_seconds(self) -> float | None: ...
    def history(self, seconds: float) -> list[dict[str, Any]]: ...
    def history_capacity(self) -> int: ...
    def meta(self) -> dict[str, Any]: ...
    def subscribe(self) -> Queue[dict[str, Any]]: ...
    def unsubscribe(self, queue: Queue[dict[str, Any]]) -> None: ...


class Sampler:
    """Owns the collectors, the sampling thread, the history and the subscriber fan-out."""

    def __init__(self, config: AgentConfig, device_id: str, display_name: str) -> None:
        self.config = config
        self.device_id = device_id
        self.display_name = display_name

        temperature = TemperatureCollector(
            enabled=config.temperature.enabled,
            use_psutil=config.temperature.use_psutil,
            use_sysfs=config.temperature.use_sysfs,
            use_vcgencmd=config.temperature.use_vcgencmd,
            use_sensors_json=config.temperature.use_sensors_json,
            vcgencmd_path=config.temperature.vcgencmd_path,
            sensors_path=config.temperature.sensors_path,
            preferred_labels=config.temperature.preferred_labels,
        )
        self._host = HostCollector(device_id, display_name)
        self._cpu = CpuCollector(temperature)
        self._memory = MemoryCollector()
        self._storage = StorageCollector(
            include=config.storage.include,
            exclude=config.storage.exclude,
            include_pseudo=config.storage.include_pseudo,
            min_total_bytes=config.storage.min_total_bytes,
        )
        self._network = NetworkCollector()
        self._optional = OptionalCollector()

        self._sequence = 0
        self._latest: dict[str, Any] | None = None
        self._latest_monotonic: float = 0.0
        self._history = HistoryBuffer(
            capacity_for(config.agent.history_seconds, config.agent.sample_interval)
        )
        self._subscribers: set[Queue[dict[str, Any]]] = set()
        self._subscriber_lock = threading.Lock()
        self._publishers: list[Callable[[dict[str, Any]], None]] = []

        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    # ---------------------------------------------------------------- lifecycle

    def start(self) -> None:
        if self._thread is not None:
            return
        self._cpu.prime()
        self._network.collect()  # establish the rate baseline
        self.sample_once()  # serve real data from the first request onward
        self._thread = threading.Thread(target=self._run, name="fleetpanel-sampler", daemon=True)
        self._thread.start()
        _log.info(
            "sampler started",
            extra={"fields": {"interval": self.config.agent.sample_interval,
                              "history_slots": self._history.capacity}},
        )

    def stop(self, timeout: float = 5.0) -> None:
        self._stop.set()
        thread = self._thread
        if thread is not None:
            thread.join(timeout=timeout)
            self._thread = None
        _log.info("sampler stopped")

    def _run(self) -> None:
        interval = self.config.agent.sample_interval
        next_tick = time.monotonic() + interval
        while not self._stop.is_set():
            delay = max(0.0, next_tick - time.monotonic())
            if self._stop.wait(delay):
                break
            try:
                self.sample_once()
            except Exception:  # noqa: BLE001 - the sampler thread must never die
                log_throttled(
                    _log, logging.ERROR, "sampler.loop", "sampling iteration failed", exc_info=True
                )
            now = time.monotonic()
            # Skip missed ticks rather than accumulating a backlog after a suspend.
            next_tick += interval
            if next_tick < now:
                next_tick = now + interval

    # ------------------------------------------------------------------ sampling

    def _guard(self, name: str, fn: Callable[[], dict[str, Any]], fallback: dict[str, Any],
               failures: list[str]) -> dict[str, Any]:
        try:
            return fn()
        except Exception:  # noqa: BLE001 - one bad sensor must not lose the sample
            failures.append(name)
            log_throttled(
                _log, logging.WARNING, f"collector.{name}", f"{name} collector failed",
                exc_info=True,
            )
            return dict(fallback)

    def sample_once(self) -> dict[str, Any]:
        failures: list[str] = []
        cpu = self._guard("cpu", self._cpu.collect, _NULL_CPU, failures)
        memory = self._guard("memory", self._memory.collect, _NULL_MEMORY, failures)
        storage = self._guard("storage", self._storage.collect, _NULL_STORAGE, failures)
        network = self._guard("network", self._network.collect, _NULL_NETWORK, failures)
        optional = self._guard(
            "optional", self._optional.collect, {"gpu": None, "battery": None}, failures
        )
        try:
            device = self._host.device()
        except Exception:  # noqa: BLE001
            failures.append("host")
            device = {
                "id": self.device_id,
                "name": self.display_name,
                "hostname": "unknown",
                "platform": "linux",
                "os_name": None,
                "os_version": None,
                "kernel": None,
                "architecture": None,
                "hardware_model": None,
                "agent_version": AGENT_VERSION,
            }
        status = self._guard(
            "status",
            lambda: self._host.status(degraded=bool(failures)),
            {
                "state": "degraded",
                "uptime_seconds": None,
                "boot_time": None,
                "process_count": None,
                "logged_in_users": None,
            },
            failures,
        )
        if failures:
            status["state"] = "degraded"

        self._sequence += 1
        sample: dict[str, Any] = {
            "schema": TELEMETRY_SCHEMA,
            "timestamp": utc_now_iso(),
            "sequence": self._sequence,
            "device": device,
            "status": status,
            "cpu": cpu,
            "memory": memory,
            "storage": storage,
            "network": network,
            "optional": optional,
            "capabilities": [],
        }
        sample["capabilities"] = derive_capabilities(sample)

        now = time.monotonic()
        self._latest = sample
        self._latest_monotonic = now
        self._history.append(now, sample)
        self._fan_out(sample)
        return sample

    # ------------------------------------------------------------------- readers

    def latest(self) -> dict[str, Any] | None:
        return self._latest

    def latest_age_seconds(self) -> float | None:
        if self._latest is None:
            return None
        return round(time.monotonic() - self._latest_monotonic, 3)

    def history(self, seconds: float) -> list[dict[str, Any]]:
        return self._history.since(time.monotonic(), seconds)

    def history_capacity(self) -> int:
        return self._history.capacity

    def meta(self) -> dict[str, Any]:
        """The retained MQTT ``meta`` document, also used by ``GET /api/v1/info``."""
        latest = self._latest or {}
        device = latest.get("device", {})
        from .collectors.network import ipv4_addresses  # local import: keeps startup light

        return {
            "schema": META_SCHEMA,
            "id": self.device_id,
            "name": self.display_name,
            "hostname": device.get("hostname"),
            "platform": device.get("platform", "linux"),
            "agent_version": AGENT_VERSION,
            "telemetry_schema": TELEMETRY_SCHEMA,
            "http": {
                "enabled": self.config.http.enabled,
                "port": self.config.http.port,
                "path": "/api/v1/telemetry",
                "auth": "none" if self.config.http.auth_mode == "none" else "token",
                "addresses": ipv4_addresses(),
            },
            "sample_interval_seconds": self.config.agent.sample_interval,
            "capabilities": latest.get("capabilities", []),
        }

    # ------------------------------------------------------------- notifications

    def add_publisher(self, publisher: Callable[[dict[str, Any]], None]) -> None:
        """Register a sink (MQTT) called with every new sample."""
        self._publishers.append(publisher)

    def _fan_out(self, sample: dict[str, Any]) -> None:
        for publisher in self._publishers:
            try:
                publisher(sample)
            except Exception:  # noqa: BLE001 - a broken sink must not stop sampling
                log_throttled(
                    _log, logging.WARNING, "sampler.publish", "publisher failed", exc_info=True
                )
        with self._subscriber_lock:
            targets = list(self._subscribers)
        for queue in targets:
            try:
                queue.put_nowait(sample)
            except Full:
                # Drop the oldest frame so a stalled SSE client sees fresh data on
                # recovery instead of a backlog.
                try:
                    queue.get_nowait()
                    queue.put_nowait(sample)
                except (Empty, Full):
                    pass

    def subscribe(self) -> Queue[dict[str, Any]]:
        queue: Queue[dict[str, Any]] = Queue(maxsize=_SUBSCRIBER_QUEUE_SIZE)
        with self._subscriber_lock:
            self._subscribers.add(queue)
        return queue

    def unsubscribe(self, queue: Queue[dict[str, Any]]) -> None:
        with self._subscriber_lock:
            self._subscribers.discard(queue)

    def subscriber_count(self) -> int:
        with self._subscriber_lock:
            return len(self._subscribers)

    def stream(self, timeout: float = 30.0) -> Iterator[dict[str, Any]]:
        """Blocking generator over new samples. Used by the SSE endpoint."""
        queue = self.subscribe()
        try:
            while not self._stop.is_set():
                try:
                    yield queue.get(timeout=timeout)
                except Empty:
                    return
        finally:
            self.unsubscribe(queue)
