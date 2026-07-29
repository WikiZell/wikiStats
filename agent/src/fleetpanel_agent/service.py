"""Process wiring: config -> sampler -> HTTP + mDNS + MQTT, with clean shutdown.

Startup order matters. The sampler is started first and takes one sample
synchronously, so the very first HTTP request and the first MQTT publish carry real
data. Discovery is advertised last, after the socket is listening, so a panel that
reacts instantly to the mDNS announcement never hits a closed port.
"""

from __future__ import annotations

import contextlib
import signal
import threading
from types import FrameType
from typing import Any

import uvicorn

from . import AGENT_VERSION
from .api import create_app
from .config import AgentConfig, ConfigError, load_config
from .discovery import DiscoveryAdvertiser
from .identity import resolve_identity
from .logging_setup import configure_logging, get_logger
from .mqtt import MqttPublisher
from .sampler import Sampler

_log = get_logger("service")


class AgentService:
    def __init__(self, config: AgentConfig) -> None:
        self.config = config
        self.device_id, self.display_name = resolve_identity(
            config.agent.device_id, config.agent.name
        )
        self.sampler = Sampler(config, self.device_id, self.display_name)
        self.mqtt = MqttPublisher(config, self.device_id, self.sampler.meta)
        self.discovery = DiscoveryAdvertiser(config, self.device_id, self.display_name)
        self.app = create_app(config, self.sampler)
        self._server: uvicorn.Server | None = None
        self._stopping = threading.Event()

    # ---------------------------------------------------------------- lifecycle

    def start_background(self) -> None:
        """Everything except the HTTP server."""
        self.sampler.start()
        if self.config.mqtt.enabled:
            self.mqtt.start()
            self.sampler.add_publisher(self.mqtt.publish_telemetry)

    def stop(self) -> None:
        if self._stopping.is_set():
            return
        self._stopping.set()
        _log.info("shutting down")
        with contextlib.suppress(Exception):
            self.discovery.stop()
        with contextlib.suppress(Exception):
            self.mqtt.stop()
        with contextlib.suppress(Exception):
            self.sampler.stop()

    def run(self) -> None:
        self.start_background()
        if not self.config.http.enabled:
            _log.info("HTTP disabled; running MQTT-only")
            self._run_headless()
            return

        uvicorn_config = uvicorn.Config(
            self.app,
            host=self.config.http.host,
            port=self.config.http.port,
            log_config=None,
            access_log=False,
            timeout_graceful_shutdown=5,
        )
        server = uvicorn.Server(uvicorn_config)
        self._server = server

        # uvicorn installs its own SIGINT/SIGTERM handling; hook our teardown onto it
        # so mDNS unregisters and MQTT publishes 'offline' before the process exits.
        original_handler = server.handle_exit

        def handle_exit(sig: int, frame: FrameType | None) -> None:
            self.stop()
            original_handler(sig, frame)

        server.handle_exit = handle_exit  # type: ignore[method-assign]

        announcer = threading.Thread(
            target=self._announce_when_listening, args=(server,), daemon=True
        )
        announcer.start()
        _log.info(
            "starting HTTP server",
            extra={"fields": {"host": self.config.http.host, "port": self.config.http.port,
                              "auth": self.config.http.auth_mode}},
        )
        try:
            server.run()
        finally:
            self.stop()

    def _announce_when_listening(self, server: uvicorn.Server, timeout: float = 15.0) -> None:
        deadline = threading.Event()
        waited = 0.0
        while not server.started and waited < timeout and not self._stopping.is_set():
            deadline.wait(0.1)
            waited += 0.1
        if server.started and not self._stopping.is_set():
            self.discovery.start()
            self.log_endpoints()

    def _run_headless(self) -> None:
        stop = threading.Event()

        def handler(_signum: int, _frame: FrameType | None) -> None:
            stop.set()

        with contextlib.suppress(ValueError):  # not the main thread in tests
            signal.signal(signal.SIGINT, handler)
            signal.signal(signal.SIGTERM, handler)
        try:
            while not stop.is_set():
                stop.wait(1.0)
        finally:
            self.stop()

    # ------------------------------------------------------------------ helpers

    def log_endpoints(self) -> None:
        from .collectors.network import ipv4_addresses

        for address in ipv4_addresses() or ["<no address>"]:
            _log.info(
                "telemetry endpoint",
                extra={"fields": {"url": f"http://{address}:{self.config.http.port}/api/v1/telemetry"}},
            )

    def describe(self) -> dict[str, Any]:
        return {
            "agent_version": AGENT_VERSION,
            "device_id": self.device_id,
            "name": self.display_name,
            "http_port": self.config.http.port,
            "mqtt_enabled": self.config.mqtt.enabled,
            "discovery": self.discovery.describe(),
        }


def build_service(config_path: str | None = None) -> AgentService:
    config = load_config(config_path)
    configure_logging(config.logging.level, config.logging.format)
    return AgentService(config)


def main(config_path: str | None = None) -> int:
    try:
        service = build_service(config_path)
    except ConfigError as exc:
        configure_logging("INFO", "text")
        get_logger("config").error(str(exc))
        return 2
    service.run()
    return 0
