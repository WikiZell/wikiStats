"""TOML configuration, validated with pydantic.

Design rules:

* Every setting has a working default, so an empty config file yields a functional
  LAN agent.
* Validation happens once at startup and raises :class:`ConfigError` with a message
  naming the offending key. There is no silent fallback for a malformed value - a
  typo in ``config.toml`` must be visible, not ignored.
* Secrets live only in this object. Nothing that renders configuration for an API
  response may read the secret fields directly; use :meth:`AgentConfig.public_view`.
"""

from __future__ import annotations

import ipaddress
import tomllib
from pathlib import Path
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError, field_validator

DEFAULT_CONFIG_PATH = Path("/etc/fleetpanel-agent/config.toml")

AuthMode = Literal["none", "bearer", "query"]
LogFormat = Literal["text", "json"]
LogLevel = Literal["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]


class ConfigError(RuntimeError):
    """Raised when the configuration file is unusable."""


class _Base(BaseModel):
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)


class AgentSection(_Base):
    name: str = ""
    """Display name. Empty means 'use the hostname'."""

    device_id: str = ""
    """Override for the derived stable device ID. Empty means 'derive it'."""

    sample_interval: float = Field(default=2.0, ge=0.5, le=600.0)
    history_seconds: int = Field(default=900, ge=0, le=86400)

    @field_validator("device_id")
    @classmethod
    def _check_device_id(cls, value: str) -> str:
        if not value:
            return value
        cleaned = value.strip().lower()
        is_hex = all(c in "0123456789abcdef" for c in cleaned)
        if not (8 <= len(cleaned) <= 32) or not is_hex:
            raise ValueError("agent.device_id must be 8-32 lowercase hex characters")
        return cleaned


class HttpSection(_Base):
    enabled: bool = True
    host: str = "0.0.0.0"  # noqa: S104 - a LAN telemetry agent is meant to be reachable
    port: int = Field(default=8770, ge=1, le=65535)
    auth_mode: AuthMode = "none"
    api_token: str = ""
    trusted_networks: list[str] = Field(default_factory=list)
    cors_origins: list[str] = Field(default_factory=list)
    enable_docs: bool = True

    @field_validator("trusted_networks")
    @classmethod
    def _check_networks(cls, value: list[str]) -> list[str]:
        for item in value:
            try:
                ipaddress.ip_network(item, strict=False)
            except ValueError as exc:
                raise ValueError(
                    f"http.trusted_networks entry {item!r} is not a CIDR: {exc}"
                ) from exc
        return value


class StorageSection(_Base):
    include: list[str] = Field(default_factory=list)
    """Explicit mountpoint allow-list. Empty means 'every real filesystem'."""

    exclude: list[str] = Field(default_factory=list)
    """Mountpoints (or mountpoint prefixes) to drop even when they look real."""

    include_pseudo: bool = False
    min_total_bytes: int = Field(default=64 * 1024 * 1024, ge=0)
    """Filesystems smaller than this are ignored; they are almost always overlays."""


class TemperatureSection(_Base):
    enabled: bool = True
    use_psutil: bool = True
    use_sysfs: bool = True
    use_vcgencmd: bool = True
    use_sensors_json: bool = False
    vcgencmd_path: str = "/usr/bin/vcgencmd"
    sensors_path: str = "/usr/bin/sensors"
    preferred_labels: list[str] = Field(
        default_factory=lambda: [
            "coretemp",
            "k10temp",
            "cpu_thermal",
            "soc_thermal",
            "package",
            "tctl",
            "cpu-thermal",
            "x86_pkg_temp",
            "cpu",
        ]
    )


class DiscoverySection(_Base):
    enabled: bool = True
    service_name: str = ""
    """Instance name for mDNS. Empty means 'use the display name'."""


class MqttSection(_Base):
    enabled: bool = False
    host: str = ""
    port: int = Field(default=1883, ge=1, le=65535)
    username: str = ""
    password: str = ""
    tls: bool = False
    tls_ca_file: str = ""
    tls_insecure: bool = False
    base_topic: str = "fleetpanel/v1"
    telemetry_qos: int = Field(default=0, ge=0, le=1)
    telemetry_retain: bool = True
    keepalive: int = Field(default=30, ge=5, le=3600)
    max_backoff_seconds: float = Field(default=60.0, ge=1.0, le=3600.0)

    @field_validator("base_topic")
    @classmethod
    def _check_topic(cls, value: str) -> str:
        cleaned = value.strip().strip("/")
        if not cleaned:
            raise ValueError("mqtt.base_topic must not be empty")
        if "+" in cleaned or "#" in cleaned:
            raise ValueError("mqtt.base_topic must not contain wildcards")
        return cleaned


class LoggingSection(_Base):
    level: LogLevel = "INFO"
    format: LogFormat = "text"
    throttle_seconds: float = Field(default=300.0, ge=1.0, le=86400.0)


class AgentConfig(_Base):
    agent: AgentSection = Field(default_factory=AgentSection)
    http: HttpSection = Field(default_factory=HttpSection)
    storage: StorageSection = Field(default_factory=StorageSection)
    temperature: TemperatureSection = Field(default_factory=TemperatureSection)
    discovery: DiscoverySection = Field(default_factory=DiscoverySection)
    mqtt: MqttSection = Field(default_factory=MqttSection)
    logging: LoggingSection = Field(default_factory=LoggingSection)

    def validate_cross_section(self) -> None:
        """Checks that need more than one section. Raises :class:`ConfigError`."""
        if self.http.auth_mode in ("bearer", "query") and not self.http.api_token:
            raise ConfigError(
                f"http.auth_mode = '{self.http.auth_mode}' requires a non-empty http.api_token"
            )
        if self.http.auth_mode == "none" and self.http.api_token:
            # Not fatal, but it means the operator believes the agent is protected.
            raise ConfigError(
                "http.api_token is set but http.auth_mode = 'none'; "
                "set auth_mode = 'bearer' or clear the token"
            )
        if len(self.http.api_token) and len(self.http.api_token) < 16:
            raise ConfigError("http.api_token must be at least 16 characters")
        if self.mqtt.enabled and not self.mqtt.host:
            raise ConfigError("mqtt.enabled = true requires mqtt.host")
        if self.mqtt.tls and self.mqtt.tls_ca_file and not Path(self.mqtt.tls_ca_file).is_file():
            raise ConfigError(f"mqtt.tls_ca_file not found: {self.mqtt.tls_ca_file}")
        if not self.http.enabled and not self.mqtt.enabled:
            raise ConfigError(
                "both http.enabled and mqtt.enabled are false; the agent would do nothing"
            )

    def public_view(self) -> dict[str, Any]:
        """Configuration safe for ``GET /api/v1/config/public``.

        Secrets are reported as booleans so an operator can tell that a token is
        configured without the value leaving the machine.
        """
        return {
            "agent": {
                "name": self.agent.name,
                "sample_interval": self.agent.sample_interval,
                "history_seconds": self.agent.history_seconds,
            },
            "http": {
                "port": self.http.port,
                "auth_mode": self.http.auth_mode,
                "api_token_set": bool(self.http.api_token),
                "trusted_networks": list(self.http.trusted_networks),
                "docs_enabled": self.http.enable_docs,
            },
            "storage": {
                "include": list(self.storage.include),
                "exclude": list(self.storage.exclude),
                "include_pseudo": self.storage.include_pseudo,
            },
            "temperature": {
                "enabled": self.temperature.enabled,
                "sources": {
                    "psutil": self.temperature.use_psutil,
                    "sysfs": self.temperature.use_sysfs,
                    "vcgencmd": self.temperature.use_vcgencmd,
                    "sensors_json": self.temperature.use_sensors_json,
                },
            },
            "discovery": {"enabled": self.discovery.enabled},
            "mqtt": {
                "enabled": self.mqtt.enabled,
                "host": self.mqtt.host,
                "port": self.mqtt.port,
                "tls": self.mqtt.tls,
                "base_topic": self.mqtt.base_topic,
                "telemetry_qos": self.mqtt.telemetry_qos,
                "telemetry_retain": self.mqtt.telemetry_retain,
                "username_set": bool(self.mqtt.username),
                "password_set": bool(self.mqtt.password),
            },
            "logging": {"level": self.logging.level, "format": self.logging.format},
        }


def load_config(path: str | Path | None = None) -> AgentConfig:
    """Read and validate ``config.toml``.

    A missing file is not an error - the agent starts with defaults, which is what a
    fresh install wants. A malformed or invalid file *is* an error.
    """
    if path is None:
        path = DEFAULT_CONFIG_PATH
    path = Path(path)
    if not path.exists():
        config = AgentConfig()
        config.validate_cross_section()
        return config
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise ConfigError(f"cannot read {path}: {exc}") from exc
    return parse_config(raw, source=str(path))


def parse_config(raw: dict[str, Any], source: str = "<memory>") -> AgentConfig:
    """Validate an already-parsed TOML mapping."""
    try:
        config = AgentConfig.model_validate(raw)
    except ValidationError as exc:
        details = "; ".join(
            f"{'.'.join(str(p) for p in err['loc'])}: {err['msg']}" for err in exc.errors()
        )
        raise ConfigError(f"invalid configuration in {source}: {details}") from exc
    config.validate_cross_section()
    return config
