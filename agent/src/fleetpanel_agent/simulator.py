"""Development telemetry simulator.

Spawns N fake machines, each with its own HTTP port, its own mDNS advertisement and,
optionally, its own MQTT publications. It exists so multi-device panel work - the
carousel, device ordering, dedup, offline handling - can be exercised without owning
five computers.

    python -m fleetpanel_agent.simulator --devices 5 --port-start 8800

Everything it emits is synthetic and clearly labelled: hostnames are ``sim-XX``,
``hardware_model`` says ``FleetPanel Simulator``, and the device IDs are derived from
a fixed salt so they are stable across restarts (the panel must not see a new device
every time the simulator is restarted). This is the only place in the project that
fabricates telemetry values.
"""

# The randomness here shapes fake dashboards, never a secret. `random` is correct.
# ruff: noqa: S311

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import math
import random
import signal
import socket
import threading
import time
from queue import Empty, Full, Queue
from types import FrameType
from typing import Any

import uvicorn

from . import AGENT_VERSION, MDNS_SERVICE_TYPE, META_SCHEMA, TELEMETRY_SCHEMA
from .api import create_app
from .config import AgentConfig
from .logging_setup import configure_logging, get_logger
from .ringbuffer import HistoryBuffer, capacity_for
from .sampler import derive_capabilities, utc_now_iso

_log = get_logger("simulator")

_SIM_SALT = b"fleetpanel.simulator.v1"

_PROFILES: tuple[dict[str, Any], ...] = (
    {
        "os_name": "Debian GNU/Linux 12 (bookworm)",
        "os_version": "12",
        "kernel": "6.6.31+rpt-rpi-v8",
        "architecture": "aarch64",
        "hardware_model": "Raspberry Pi 4 Model B Rev 1.5 (FleetPanel Simulator)",
        "physical_cores": 4,
        "logical_cores": 4,
        "base_mhz": 1500.0,
        "ram": 4 * 1024**3,
        "disk": 32 * 1024**3,
        "base_temp": 46.0,
    },
    {
        "os_name": "Ubuntu 24.04.1 LTS",
        "os_version": "24.04",
        "kernel": "6.8.0-45-generic",
        "architecture": "x86_64",
        "hardware_model": "Dell Inc. OptiPlex 7070 (FleetPanel Simulator)",
        "physical_cores": 6,
        "logical_cores": 12,
        "base_mhz": 3200.0,
        "ram": 16 * 1024**3,
        "disk": 512 * 1024**3,
        "base_temp": 41.0,
    },
    {
        "os_name": "Raspberry Pi OS Lite (64-bit)",
        "os_version": "12",
        "kernel": "6.6.51+rpt-rpi-2712",
        "architecture": "aarch64",
        "hardware_model": "Raspberry Pi 5 Model B Rev 1.0 (FleetPanel Simulator)",
        "physical_cores": 4,
        "logical_cores": 4,
        "base_mhz": 2400.0,
        "ram": 8 * 1024**3,
        "disk": 128 * 1024**3,
        "base_temp": 52.0,
    },
    {
        "os_name": "Debian GNU/Linux 13 (trixie)",
        "os_version": "13",
        "kernel": "6.12.9-amd64",
        "architecture": "x86_64",
        "hardware_model": "Supermicro X11SSH-F (FleetPanel Simulator)",
        "physical_cores": 8,
        "logical_cores": 16,
        "base_mhz": 2800.0,
        "ram": 64 * 1024**3,
        "disk": 4 * 1024**4,
        "base_temp": 38.0,
    },
)


def simulated_device_id(index: int) -> str:
    """Stable across restarts so the panel keeps its device list."""
    return hashlib.sha256(_SIM_SALT + str(index).encode()).hexdigest()[:12]


class FakeMachine:
    """One synthetic host with smoothly varying, self-consistent metrics."""

    def __init__(self, index: int, seed: int | None = None) -> None:
        self.index = index
        self.profile = _PROFILES[index % len(_PROFILES)]
        self.device_id = simulated_device_id(index)
        self.hostname = f"sim-{index:02d}"
        self.name = f"Sim {index:02d}"
        self._rng = random.Random(seed if seed is not None else index * 7919)
        self._sequence = 0
        self._t0 = time.time() - self._rng.randint(3_600, 900_000)
        # Phase offsets keep the fleet from breathing in unison, which would make a
        # carousel demo look fake.
        self._phase = self._rng.random() * math.tau
        self._cpu = self._rng.uniform(5.0, 30.0)
        self._ram_used = self.profile["ram"] * self._rng.uniform(0.25, 0.55)
        self._disk_used = self.profile["disk"] * self._rng.uniform(0.30, 0.75)
        self._rx_total = self._rng.randint(10**9, 10**11)
        self._tx_total = self._rng.randint(10**8, 10**10)

    # ------------------------------------------------------------------ physics

    def _walk(self, value: float, low: float, high: float, step: float) -> float:
        value += self._rng.uniform(-step, step)
        # Gentle pull toward the middle so a long run does not park at a rail.
        centre = (low + high) / 2.0
        value += (centre - value) * 0.02
        return max(low, min(high, value))

    def tick(self, elapsed: float) -> dict[str, Any]:
        now = time.time()
        wave = math.sin(now / 37.0 + self._phase)
        self._cpu = self._walk(self._cpu + wave * 1.5, 1.0, 99.0, 6.0)
        self._ram_used = self._walk(
            self._ram_used, self.profile["ram"] * 0.15, self.profile["ram"] * 0.95,
            self.profile["ram"] * 0.01,
        )
        self._disk_used = self._walk(
            self._disk_used, self.profile["disk"] * 0.10, self.profile["disk"] * 0.97,
            self.profile["disk"] * 0.0005,
        )
        rx_rate = max(0.0, self._rng.gauss(180_000 * (1 + self._cpu / 100), 90_000))
        tx_rate = max(0.0, self._rng.gauss(40_000 * (1 + self._cpu / 100), 25_000))
        self._rx_total += int(rx_rate * elapsed)
        self._tx_total += int(tx_rate * elapsed)

        cores = int(self.profile["logical_cores"])
        per_core = [
            round(max(0.0, min(100.0, self._rng.gauss(self._cpu, 9.0))), 1) for _ in range(cores)
        ]
        # Temperature tracks load with thermal lag, which is what makes the panel's
        # warning thresholds worth testing.
        temp = self.profile["base_temp"] + self._cpu * 0.32 + self._rng.uniform(-0.6, 0.6)

        ram_total = int(self.profile["ram"])
        ram_used = int(self._ram_used)
        ram_available = ram_total - ram_used
        swap_total = 1024**3 // 2
        swap_used = int(swap_total * min(0.9, max(0.0, (self._cpu - 60) / 100)))

        disk_total = int(self.profile["disk"])
        disk_used = int(self._disk_used)

        self._sequence += 1
        sample: dict[str, Any] = {
            "schema": TELEMETRY_SCHEMA,
            "timestamp": utc_now_iso(),
            "sequence": self._sequence,
            "device": {
                "id": self.device_id,
                "name": self.name,
                "hostname": self.hostname,
                "platform": "linux",
                "os_name": self.profile["os_name"],
                "os_version": self.profile["os_version"],
                "kernel": self.profile["kernel"],
                "architecture": self.profile["architecture"],
                "hardware_model": self.profile["hardware_model"],
                "agent_version": AGENT_VERSION,
            },
            "status": {
                "state": "online",
                "uptime_seconds": int(now - self._t0),
                "boot_time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(self._t0)),
                "process_count": 90 + self.index * 7 + int(self._cpu / 4),
                "logged_in_users": 1 if self.index % 3 else 0,
            },
            "cpu": {
                "usage_percent": round(self._cpu, 1),
                "per_core_percent": per_core,
                "physical_cores": int(self.profile["physical_cores"]),
                "logical_cores": cores,
                "frequency_mhz": round(
                    float(self.profile["base_mhz"]) * (0.6 + 0.4 * self._cpu / 100), 1
                ),
                "load_1": round(cores * self._cpu / 100.0, 2),
                "load_5": round(cores * self._cpu / 115.0, 2),
                "load_15": round(cores * self._cpu / 130.0, 2),
                "temperature_c": round(temp, 1),
                "temperatures": [
                    {
                        "label": "cpu_thermal",
                        "temperature_c": round(temp, 1),
                        "high_c": 80.0,
                        "critical_c": 90.0,
                    }
                ],
            },
            "memory": {
                "total_bytes": ram_total,
                "available_bytes": ram_available,
                "used_bytes": ram_used,
                "free_bytes": max(0, ram_available - ram_total // 8),
                "usage_percent": round(ram_used / ram_total * 100, 1),
                "swap_total_bytes": swap_total,
                "swap_used_bytes": swap_used,
                "swap_free_bytes": swap_total - swap_used,
                "swap_usage_percent": round(swap_used / swap_total * 100, 1),
            },
            "storage": {
                "total_bytes": disk_total,
                "used_bytes": disk_used,
                "free_bytes": disk_total - disk_used,
                "usage_percent": round(disk_used / disk_total * 100, 1),
                "mounts": [
                    {
                        "device": "/dev/sim0p2",
                        "mountpoint": "/",
                        "filesystem": "ext4",
                        "total_bytes": disk_total,
                        "used_bytes": disk_used,
                        "free_bytes": disk_total - disk_used,
                        "usage_percent": round(disk_used / disk_total * 100, 1),
                    }
                ],
            },
            "network": {
                "primary_interface": "eth0" if self.index % 2 == 0 else "wlan0",
                "ip_addresses": [f"10.99.{self.index // 250}.{(self.index % 250) + 2}"],
                "rx_bytes_total": self._rx_total,
                "tx_bytes_total": self._tx_total,
                "rx_bytes_per_second": round(rx_rate, 1),
                "tx_bytes_per_second": round(tx_rate, 1),
            },
            "optional": {"gpu": None, "battery": None},
            "capabilities": [],
        }
        sample["capabilities"] = derive_capabilities(sample)
        return sample


class SimulatedSource:
    """Implements ``TelemetrySource`` on top of a :class:`FakeMachine`."""

    def __init__(self, machine: FakeMachine, config: AgentConfig) -> None:
        self.machine = machine
        self.config = config
        self._latest: dict[str, Any] | None = None
        self._latest_monotonic = 0.0
        self._history = HistoryBuffer(
            capacity_for(config.agent.history_seconds, config.agent.sample_interval)
        )
        self._subscribers: set[Queue[dict[str, Any]]] = set()
        self._lock = threading.Lock()

    def tick(self, elapsed: float) -> dict[str, Any]:
        sample = self.machine.tick(elapsed)
        now = time.monotonic()
        self._latest = sample
        self._latest_monotonic = now
        self._history.append(now, sample)
        with self._lock:
            targets = list(self._subscribers)
        for queue in targets:
            with contextlib.suppress(Full):
                queue.put_nowait(sample)
        return sample

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
        latest = self._latest or {}
        return {
            "schema": META_SCHEMA,
            "id": self.machine.device_id,
            "name": self.machine.name,
            "hostname": self.machine.hostname,
            "platform": "linux",
            "agent_version": AGENT_VERSION,
            "telemetry_schema": TELEMETRY_SCHEMA,
            "http": {
                "enabled": True,
                "port": self.config.http.port,
                "path": "/api/v1/telemetry",
                "auth": "none" if self.config.http.auth_mode == "none" else "token",
                "addresses": _host_addresses(),
            },
            "sample_interval_seconds": self.config.agent.sample_interval,
            "capabilities": latest.get("capabilities", []),
        }

    def subscribe(self) -> Queue[dict[str, Any]]:
        queue: Queue[dict[str, Any]] = Queue(maxsize=4)
        with self._lock:
            self._subscribers.add(queue)
        return queue

    def unsubscribe(self, queue: Queue[dict[str, Any]]) -> None:
        with self._lock:
            self._subscribers.discard(queue)
        with contextlib.suppress(Empty):
            queue.get_nowait()


def _host_addresses() -> list[str]:
    from .collectors.network import ipv4_addresses

    return ipv4_addresses()


class SimulatedFleet:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.sources: list[SimulatedSource] = []
        self.servers: list[uvicorn.Server] = []
        self.threads: list[threading.Thread] = []
        self._stop = threading.Event()
        self._zeroconf: Any = None
        self._services: list[Any] = []
        self._mqtt: Any = None

    def build(self) -> None:
        for index in range(self.args.devices):
            config = AgentConfig()
            config.agent.name = f"Sim {index:02d}"
            config.agent.sample_interval = self.args.interval
            config.agent.history_seconds = self.args.history
            config.http.port = self.args.port_start + index
            config.http.host = self.args.host
            config.http.enable_docs = True
            source = SimulatedSource(FakeMachine(index, seed=self.args.seed + index), config)
            source.tick(self.args.interval)
            app = create_app(config, source)
            server = uvicorn.Server(
                uvicorn.Config(
                    app, host=config.http.host, port=config.http.port,
                    log_config=None, access_log=False,
                )
            )
            # uvicorn only installs signal handlers when serve() runs on the main
            # thread, so these per-device servers need no patching.
            self.sources.append(source)
            self.servers.append(server)

    def start(self) -> None:
        for server in self.servers:
            thread = threading.Thread(target=server.run, daemon=True)
            thread.start()
            self.threads.append(thread)
        if self.args.mdns:
            self._start_mdns()
        if self.args.mqtt_host:
            self._start_mqtt()
        ticker = threading.Thread(target=self._tick_loop, daemon=True)
        ticker.start()
        self.threads.append(ticker)

    def _tick_loop(self) -> None:
        interval = self.args.interval
        while not self._stop.wait(interval):
            for source in self.sources:
                sample = source.tick(interval)
                if self._mqtt is not None:
                    self._publish(source, sample)

    # --------------------------------------------------------------------- mDNS

    def _start_mdns(self) -> None:
        from zeroconf import IPVersion, ServiceInfo, Zeroconf

        from .discovery import build_txt_properties, instance_name, local_addresses

        try:
            self._zeroconf = Zeroconf(ip_version=IPVersion.V4Only)
        except Exception:  # noqa: BLE001
            _log.warning("cannot start zeroconf; simulator will be HTTP-only", exc_info=True)
            return
        addresses = local_addresses()
        for source in self.sources:
            info = ServiceInfo(
                MDNS_SERVICE_TYPE,
                instance_name(source.machine.name, source.machine.device_id),
                addresses=addresses,
                port=source.config.http.port,
                properties=build_txt_properties(
                    source.machine.device_id, source.machine.name, "none"
                ),
                server=f"{socket.gethostname().split('.')[0]}-sim{source.machine.index}.local.",
            )
            try:
                self._zeroconf.register_service(info, allow_name_change=True)
                self._services.append(info)
            except Exception:  # noqa: BLE001
                _log.warning("mDNS registration failed for %s", source.machine.name)
        _log.info("advertised %d simulated devices over mDNS", len(self._services))

    # --------------------------------------------------------------------- MQTT

    def _start_mqtt(self) -> None:
        import paho.mqtt.client as mqtt
        from paho.mqtt.enums import CallbackAPIVersion

        from .mqtt import PAYLOAD_ONLINE, build_topics

        client = mqtt.Client(
            callback_api_version=CallbackAPIVersion.VERSION2,
            client_id=f"fleetpanel-simulator-{self.args.port_start}",
        )
        if self.args.mqtt_username:
            client.username_pw_set(self.args.mqtt_username, self.args.mqtt_password or None)
        try:
            client.connect(self.args.mqtt_host, self.args.mqtt_port, keepalive=30)
            client.loop_start()
        except OSError as exc:
            _log.warning("MQTT connect failed (%s); continuing HTTP-only", exc)
            return
        self._mqtt = client
        for source in self.sources:
            topics = build_topics(self.args.mqtt_base_topic, source.machine.device_id)
            client.publish(topics["availability"], PAYLOAD_ONLINE, qos=1, retain=True)
            client.publish(
                topics["meta"], json.dumps(source.meta(), separators=(",", ":")), qos=1, retain=True
            )
        _log.info("publishing %d simulated devices to MQTT", len(self.sources))

    def _publish(self, source: SimulatedSource, sample: dict[str, Any]) -> None:
        from .mqtt import build_topics

        topics = build_topics(self.args.mqtt_base_topic, source.machine.device_id)
        self._mqtt.publish(
            topics["telemetry"], json.dumps(sample, separators=(",", ":")), qos=0, retain=True
        )

    # ------------------------------------------------------------------ teardown

    def stop(self) -> None:
        self._stop.set()
        if self._mqtt is not None:
            from .mqtt import PAYLOAD_OFFLINE, build_topics

            for source in self.sources:
                topics = build_topics(self.args.mqtt_base_topic, source.machine.device_id)
                with contextlib.suppress(Exception):
                    self._mqtt.publish(topics["availability"], PAYLOAD_OFFLINE, qos=1, retain=True)
            with contextlib.suppress(Exception):
                self._mqtt.loop_stop()
                self._mqtt.disconnect()
        if self._zeroconf is not None:
            for info in self._services:
                with contextlib.suppress(Exception):
                    self._zeroconf.unregister_service(info)
            with contextlib.suppress(Exception):
                self._zeroconf.close()
        for server in self.servers:
            server.should_exit = True

    def print_endpoints(self) -> None:
        addresses = _host_addresses() or ["127.0.0.1"]
        host = addresses[0]
        for source in self.sources:
            _log.info(
                "simulated device ready",
                extra={
                    "fields": {
                        "name": source.machine.name,
                        "id": source.machine.device_id,
                        "url": f"http://{host}:{source.config.http.port}/api/v1/telemetry",
                    }
                },
            )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m fleetpanel_agent.simulator",
        description="Serve N synthetic FleetPanel devices for panel development",
    )
    parser.add_argument("--devices", type=int, default=5, help="number of fake machines")
    parser.add_argument("--port-start", type=int, default=8800, help="first HTTP port")
    parser.add_argument("--host", default="0.0.0.0", help="bind address")  # noqa: S104
    parser.add_argument("--interval", type=float, default=2.0, help="sample interval seconds")
    parser.add_argument("--history", type=int, default=300, help="history retention seconds")
    parser.add_argument("--seed", type=int, default=1, help="base RNG seed")
    parser.add_argument(
        "--mdns", action=argparse.BooleanOptionalAction, default=True,
        help="advertise each device over mDNS",
    )
    parser.add_argument("--mqtt-host", default="", help="publish to this MQTT broker")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--mqtt-username", default="")
    parser.add_argument("--mqtt-password", default="")
    parser.add_argument("--mqtt-base-topic", default="fleetpanel/v1")
    parser.add_argument("--log-level", default="INFO")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    configure_logging(args.log_level, "text")
    if args.devices < 1:
        _log.error("--devices must be at least 1")
        return 2

    fleet = SimulatedFleet(args)
    fleet.build()
    fleet.start()
    time.sleep(0.5)
    fleet.print_endpoints()

    stop = threading.Event()

    def handler(_signum: int, _frame: FrameType | None) -> None:
        stop.set()

    with contextlib.suppress(ValueError):
        signal.signal(signal.SIGINT, handler)
        signal.signal(signal.SIGTERM, handler)
    try:
        while not stop.wait(0.5):
            pass
    finally:
        fleet.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
