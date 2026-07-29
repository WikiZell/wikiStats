"""Optional MQTT publisher.

MQTT never affects REST. The client runs on paho's own network thread; a broker that
is down, slow or refusing credentials produces throttled log lines and nothing else.
Sampling and the HTTP API keep running.

Contract (see ``shared/mqtt-topics.md``):

* ``.../availability`` retained QoS 1, ``online`` on connect and ``offline`` via LWT.
* ``.../meta`` retained QoS 1, republished on every reconnect.
* ``.../telemetry`` retained, QoS configurable, one message per sample.

Passwords are never logged. Log lines report ``password=<set>`` or ``<unset>``.
"""

from __future__ import annotations

import contextlib
import json
import logging
import ssl
import threading
from typing import Any

import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion

from .config import AgentConfig
from .logging_setup import get_logger, log_throttled

_log = get_logger("mqtt")

PAYLOAD_ONLINE = "online"
PAYLOAD_OFFLINE = "offline"


def build_topics(base_topic: str, device_id: str) -> dict[str, str]:
    """Pure topic construction. Unit tested directly."""
    base = base_topic.strip().strip("/")
    root = f"{base}/devices/{device_id}"
    return {
        "meta": f"{root}/meta",
        "telemetry": f"{root}/telemetry",
        "availability": f"{root}/availability",
    }


def parse_device_topic(base_topic: str, topic: str) -> tuple[str, str] | None:
    """Inverse of :func:`build_topics`: ``(device_id, leaf)`` or ``None``.

    Shared with the panel's expectations so both sides agree on the layout; the
    firmware implements the same parse in C++.
    """
    base = base_topic.strip().strip("/")
    prefix = f"{base}/devices/"
    if not topic.startswith(prefix):
        return None
    remainder = topic[len(prefix):]
    device_id, sep, leaf = remainder.partition("/")
    if not sep or not device_id or "/" in leaf:
        return None
    if leaf not in ("meta", "telemetry", "availability"):
        return None
    return device_id, leaf


class MqttPublisher:
    def __init__(self, config: AgentConfig, device_id: str, meta_provider: Any) -> None:
        self.config = config
        self.device_id = device_id
        self.topics = build_topics(config.mqtt.base_topic, device_id)
        self._meta_provider = meta_provider
        self._client: mqtt.Client | None = None
        self._connected = threading.Event()
        self._started = False

    # ---------------------------------------------------------------- lifecycle

    def start(self) -> None:
        if not self.config.mqtt.enabled or self._started:
            return
        cfg = self.config.mqtt
        client = mqtt.Client(
            callback_api_version=CallbackAPIVersion.VERSION2,
            # Stable client ID: a random one per connect makes brokers accumulate
            # ghost sessions and can trigger disconnect loops on ID collision.
            client_id=f"fleetpanel-agent-{self.device_id}",
            clean_session=True,
        )
        if cfg.username:
            client.username_pw_set(cfg.username, cfg.password or None)
        if cfg.tls:
            client.tls_set(
                ca_certs=cfg.tls_ca_file or None,
                cert_reqs=ssl.CERT_NONE if cfg.tls_insecure else ssl.CERT_REQUIRED,
            )
            if cfg.tls_insecure:
                client.tls_insecure_set(True)

        client.will_set(self.topics["availability"], PAYLOAD_OFFLINE, qos=1, retain=True)
        # Exponential backoff with paho's own jitter, capped by configuration.
        client.reconnect_delay_set(min_delay=1, max_delay=int(cfg.max_backoff_seconds))
        client.on_connect = self._on_connect
        client.on_disconnect = self._on_disconnect

        self._client = client
        self._started = True
        _log.info(
            "connecting to MQTT broker",
            extra={
                "fields": {
                    "host": cfg.host,
                    "port": cfg.port,
                    "tls": cfg.tls,
                    "username": cfg.username or "<unset>",
                    "password": "<set>" if cfg.password else "<unset>",
                }
            },
        )
        try:
            client.connect_async(cfg.host, cfg.port, keepalive=cfg.keepalive)
            client.loop_start()
        except Exception:  # noqa: BLE001 - broker down at boot must not stop the agent
            _log.warning("initial MQTT connect failed; will keep retrying", exc_info=True)

    def stop(self) -> None:
        client = self._client
        if client is None:
            return
        try:
            if self._connected.is_set():
                # Explicit offline: a clean shutdown should not wait for the LWT.
                client.publish(self.topics["availability"], PAYLOAD_OFFLINE, qos=1, retain=True)
                client.loop_write()
            client.disconnect()
        except Exception:  # noqa: BLE001 - shutdown must not raise
            _log.debug("MQTT disconnect failed", exc_info=True)
        finally:
            with contextlib.suppress(Exception):
                client.loop_stop()
            self._client = None
            self._connected.clear()
            self._started = False

    # ---------------------------------------------------------------- callbacks

    def _on_connect(self, client: mqtt.Client, _userdata: Any, _flags: Any,
                    reason_code: Any, _properties: Any = None) -> None:
        if getattr(reason_code, "is_failure", False) or (
            isinstance(reason_code, int) and reason_code != 0
        ):
            log_throttled(
                _log, logging.WARNING, "mqtt.connect_failed",
                "MQTT connection refused", reason=str(reason_code),
            )
            self._connected.clear()
            return
        self._connected.set()
        _log.info("MQTT connected", extra={"fields": {"host": self.config.mqtt.host}})
        try:
            client.publish(self.topics["availability"], PAYLOAD_ONLINE, qos=1, retain=True)
            self.publish_meta()
        except Exception:  # noqa: BLE001
            _log.warning("post-connect publish failed", exc_info=True)

    def _on_disconnect(self, _client: mqtt.Client, _userdata: Any, *args: Any) -> None:
        self._connected.clear()
        reason = args[-2] if len(args) >= 2 else (args[0] if args else "?")
        log_throttled(
            _log, logging.WARNING, "mqtt.disconnect",
            "MQTT disconnected; paho will retry with backoff", reason=str(reason),
        )

    # ----------------------------------------------------------------- publish

    @property
    def connected(self) -> bool:
        return self._connected.is_set()

    def publish_meta(self) -> None:
        client = self._client
        if client is None:
            return
        try:
            payload = json.dumps(self._meta_provider(), separators=(",", ":"))
        except Exception:  # noqa: BLE001
            _log.warning("meta serialisation failed", exc_info=True)
            return
        client.publish(self.topics["meta"], payload, qos=1, retain=True)

    def publish_telemetry(self, sample: dict[str, Any]) -> None:
        """Sampler publisher hook. Silently drops samples while disconnected."""
        client = self._client
        if client is None or not self._connected.is_set():
            return
        try:
            payload = json.dumps(sample, separators=(",", ":"))
        except (TypeError, ValueError):
            log_throttled(_log, logging.WARNING, "mqtt.serialise", "telemetry serialisation failed")
            return
        result = client.publish(
            self.topics["telemetry"],
            payload,
            qos=self.config.mqtt.telemetry_qos,
            retain=self.config.mqtt.telemetry_retain,
        )
        if result.rc != mqtt.MQTT_ERR_SUCCESS:
            log_throttled(
                _log, logging.WARNING, "mqtt.publish", "telemetry publish failed", rc=result.rc
            )
