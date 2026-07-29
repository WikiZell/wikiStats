"""Configuration validation and stable device identity."""

from __future__ import annotations

from pathlib import Path

import pytest

from fleetpanel_agent.config import AgentConfig, ConfigError, load_config, parse_config
from fleetpanel_agent.identity import derive_device_id, read_machine_id, resolve_identity

# ================================================================== configuration


def test_empty_config_is_valid() -> None:
    config = parse_config({})
    assert config.http.port == 8770
    assert config.agent.sample_interval == 2.0
    assert config.mqtt.enabled is False


def test_missing_file_yields_defaults(tmp_path: Path) -> None:
    config = load_config(tmp_path / "absent.toml")
    assert config.http.port == 8770


def test_full_config_round_trip(tmp_path: Path) -> None:
    path = tmp_path / "config.toml"
    path.write_text(
        """
[agent]
name = "WikiHermes"
device_id = "f6a3f749c2dd"
sample_interval = 5.0

[http]
port = 9000
auth_mode = "bearer"
api_token = "0123456789abcdef0123"
trusted_networks = ["192.168.0.0/16", "10.0.0.0/8"]

[mqtt]
enabled = true
host = "broker.lan"
telemetry_qos = 1
""",
        encoding="utf-8",
    )
    config = load_config(path)
    assert config.agent.name == "WikiHermes"
    assert config.agent.device_id == "f6a3f749c2dd"
    assert config.http.port == 9000
    assert config.http.auth_mode == "bearer"
    assert config.mqtt.enabled is True
    assert config.mqtt.telemetry_qos == 1


def test_unknown_key_is_rejected() -> None:
    """A typo must be loud. Silently ignoring it would mean the setting never applies."""
    with pytest.raises(ConfigError, match="sampel_interval"):
        parse_config({"agent": {"sampel_interval": 2.0}})


def test_unknown_section_is_rejected() -> None:
    with pytest.raises(ConfigError):
        parse_config({"nonsense": {"a": 1}})


def test_bad_port_is_rejected() -> None:
    with pytest.raises(ConfigError, match="port"):
        parse_config({"http": {"port": 70000}})


def test_bad_sample_interval_is_rejected() -> None:
    with pytest.raises(ConfigError, match="sample_interval"):
        parse_config({"agent": {"sample_interval": 0.0}})


def test_bad_cidr_is_rejected() -> None:
    with pytest.raises(ConfigError, match="trusted_networks"):
        parse_config({"http": {"trusted_networks": ["192.168.1.0/99"]}})


def test_bad_auth_mode_is_rejected() -> None:
    with pytest.raises(ConfigError):
        parse_config({"http": {"auth_mode": "basic"}})


def test_bearer_without_token_is_rejected() -> None:
    with pytest.raises(ConfigError, match="api_token"):
        parse_config({"http": {"auth_mode": "bearer"}})


def test_token_without_bearer_is_rejected() -> None:
    """Otherwise the operator believes the agent is protected when it is not."""
    with pytest.raises(ConfigError, match="auth_mode"):
        parse_config({"http": {"auth_mode": "none", "api_token": "0123456789abcdef0123"}})


def test_short_token_is_rejected() -> None:
    with pytest.raises(ConfigError, match="at least 16"):
        parse_config({"http": {"auth_mode": "bearer", "api_token": "short"}})


def test_mqtt_without_host_is_rejected() -> None:
    with pytest.raises(ConfigError, match="mqtt.host"):
        parse_config({"mqtt": {"enabled": True}})


def test_mqtt_base_topic_wildcards_are_rejected() -> None:
    with pytest.raises(ConfigError, match="wildcard"):
        parse_config({"mqtt": {"base_topic": "fleetpanel/+"}})


def test_all_transports_disabled_is_rejected() -> None:
    with pytest.raises(ConfigError, match="would do nothing"):
        parse_config({"http": {"enabled": False}, "mqtt": {"enabled": False}})


def test_malformed_toml_is_reported(tmp_path: Path) -> None:
    path = tmp_path / "config.toml"
    path.write_text("[agent\nname = broken", encoding="utf-8")
    with pytest.raises(ConfigError, match="cannot read"):
        load_config(path)


def test_bad_device_id_override_is_rejected() -> None:
    with pytest.raises(ConfigError, match="device_id"):
        parse_config({"agent": {"device_id": "not-hex-at-all!"}})


def test_device_id_override_is_normalised() -> None:
    config = parse_config({"agent": {"device_id": "F6A3F749C2DD"}})
    assert config.agent.device_id == "f6a3f749c2dd"


def test_public_view_hides_secrets() -> None:
    config = parse_config(
        {
            "http": {"auth_mode": "bearer", "api_token": "supersecrettoken1234"},
            "mqtt": {"enabled": True, "host": "broker.lan", "username": "u", "password": "p"},
        }
    )
    rendered = repr(config.public_view())
    assert "supersecrettoken1234" not in rendered
    assert "\"p\"" not in rendered and "'p'" not in rendered
    view = config.public_view()
    assert view["http"]["api_token_set"] is True
    assert view["mqtt"]["password_set"] is True
    assert "password" not in view["mqtt"]
    assert "api_token" not in view["http"]


def test_example_config_is_valid() -> None:
    """The shipped example must load; a broken example breaks every install."""
    example = Path(__file__).resolve().parents[1] / "packaging" / "config.example.toml"
    config = load_config(example)
    assert config.http.port == 8770
    assert config.temperature.use_vcgencmd is True
    assert "coretemp" in config.temperature.preferred_labels


# ====================================================================== identity


def test_device_id_is_stable_for_the_same_machine_id() -> None:
    first = derive_device_id("2c1e3a4b5d6f7089abcdef0123456789")
    second = derive_device_id("2c1e3a4b5d6f7089abcdef0123456789")
    assert first == second
    assert len(first) == 12
    assert all(c in "0123456789abcdef" for c in first)


def test_device_id_differs_between_machines() -> None:
    assert derive_device_id("aaaa") != derive_device_id("bbbb")


def test_device_id_does_not_leak_the_machine_id() -> None:
    machine_id = "2c1e3a4b5d6f7089abcdef0123456789"
    device_id = derive_device_id(machine_id)
    assert device_id not in machine_id
    assert machine_id[:12] != device_id


def test_device_id_falls_back_when_no_machine_id(tmp_path: Path) -> None:
    assert read_machine_id((tmp_path / "machine-id",)) is None
    device_id = derive_device_id(None, fallback_seed="mac:001122334455")
    assert len(device_id) == 12
    assert device_id == derive_device_id(None, fallback_seed="mac:001122334455")


def test_machine_id_read_from_first_existing_path(tmp_path: Path) -> None:
    first = tmp_path / "etc-machine-id"
    second = tmp_path / "dbus-machine-id"
    second.write_text("fallbackvalue\n")
    assert read_machine_id((first, second)) == "fallbackvalue"
    first.write_text("preferredvalue\n")
    assert read_machine_id((first, second)) == "preferredvalue"


def test_empty_machine_id_file_is_skipped(tmp_path: Path) -> None:
    empty = tmp_path / "empty"
    empty.write_text("\n")
    good = tmp_path / "good"
    good.write_text("realvalue")
    assert read_machine_id((empty, good)) == "realvalue"


def test_resolve_identity_uses_overrides() -> None:
    device_id, name = resolve_identity("ABCDEF012345", "  My Pi  ")
    assert device_id == "abcdef012345"
    assert name == "My Pi"


def test_resolve_identity_derives_when_unset() -> None:
    device_id, name = resolve_identity("", "")
    assert len(device_id) == 12
    assert name


def test_display_name_is_length_capped() -> None:
    _, name = resolve_identity("", "x" * 200)
    assert len(name) == 64


def test_config_defaults_survive_a_round_trip_through_model_dump() -> None:
    config = AgentConfig()
    assert parse_config(config.model_dump()).model_dump() == config.model_dump()
