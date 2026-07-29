"""mDNS metadata, MQTT topic construction, history buffer limits and sampler policy."""

from __future__ import annotations

from typing import Any

import pytest

from fleetpanel_agent import MDNS_SERVICE_TYPE
from fleetpanel_agent.config import AgentConfig, parse_config
from fleetpanel_agent.discovery import DiscoveryAdvertiser, build_txt_properties, instance_name
from fleetpanel_agent.mqtt import build_topics, parse_device_topic
from fleetpanel_agent.ringbuffer import MAX_SAMPLES, HistoryBuffer, capacity_for
from fleetpanel_agent.sampler import derive_capabilities

# ============================================================================ mDNS


def test_txt_properties_match_the_documented_contract() -> None:
    props = build_txt_properties("f6a3f749c2dd", "WikiHermes", "none")
    assert props[b"id"] == b"f6a3f749c2dd"
    assert props[b"name"] == b"WikiHermes"
    assert props[b"schema"] == b"1"
    assert props[b"path"] == b"/api/v1/telemetry"
    assert props[b"transport"] == b"http"
    assert props[b"auth"] == b"none"
    assert props[b"platform"] == b"linux"


@pytest.mark.parametrize(
    ("mode", "expected"),
    [("none", b"none"), ("bearer", b"token"), ("query", b"token")],
)
def test_txt_auth_flag(mode: str, expected: bytes) -> None:
    assert build_txt_properties("id", "name", mode)[b"auth"] == expected


def test_txt_name_is_truncated_not_rejected() -> None:
    props = build_txt_properties("id", "x" * 200, "none")
    assert len(props[b"name"]) <= 63


def test_instance_name_is_unique_and_dns_safe() -> None:
    name = instance_name("my.host.name", "f6a3f749c2dd")
    assert name.endswith(MDNS_SERVICE_TYPE)
    instance = name[: -len(MDNS_SERVICE_TYPE) - 1]
    assert "." not in instance
    assert instance.endswith("f6a3f749")


def test_instance_name_disambiguates_same_display_name() -> None:
    assert instance_name("Server", "aaaaaaaaaaaa") != instance_name("Server", "bbbbbbbbbbbb")


def test_instance_name_handles_empty_display_name() -> None:
    assert instance_name("", "abcdef012345").startswith("fleetpanel-")


def test_advertiser_describe_without_starting_the_stack() -> None:
    config = parse_config({"http": {"port": 9001}})
    described = DiscoveryAdvertiser(config, "f6a3f749c2dd", "WikiHermes").describe()
    assert described["port"] == 9001
    assert described["service_type"] == MDNS_SERVICE_TYPE
    assert described["txt"]["id"] == "f6a3f749c2dd"


def test_mdns_advertises_a_single_address(monkeypatch: pytest.MonkeyPatch) -> None:
    """Regression: several A records let each resolver pick a different one.

    An ESP32 resolving three services from one Docker/WSL host returned the LAN
    address for one and a virtual adapter's address for the others.
    """
    from fleetpanel_agent import discovery as discovery_mod
    from fleetpanel_agent.collectors import network as network_mod

    monkeypatch.setattr(
        network_mod, "ipv4_addresses", lambda: ["192.168.1.2", "172.25.208.1", "10.0.0.5"]
    )
    packed = discovery_mod.local_addresses()
    assert len(packed) == 1
    assert packed[0] == bytes([192, 168, 1, 2])

    # The full list stays available for callers that genuinely want it.
    assert len(discovery_mod.local_addresses(all_addresses=True)) == 3


def test_mdns_address_list_survives_a_host_with_no_usable_address(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from fleetpanel_agent import discovery as discovery_mod
    from fleetpanel_agent.collectors import network as network_mod

    monkeypatch.setattr(network_mod, "ipv4_addresses", lambda: [])
    assert discovery_mod.local_addresses() == []


def test_advertiser_start_is_a_noop_when_disabled() -> None:
    config = parse_config({"discovery": {"enabled": False}})
    advertiser = DiscoveryAdvertiser(config, "id", "name")
    advertiser.start()
    advertiser.stop()  # must not raise


# ============================================================================ MQTT


def test_topics_match_the_documented_contract() -> None:
    topics = build_topics("fleetpanel/v1", "f6a3f749c2dd")
    assert topics == {
        "meta": "fleetpanel/v1/devices/f6a3f749c2dd/meta",
        "telemetry": "fleetpanel/v1/devices/f6a3f749c2dd/telemetry",
        "availability": "fleetpanel/v1/devices/f6a3f749c2dd/availability",
    }


@pytest.mark.parametrize(
    "base", ["fleetpanel/v1", "/fleetpanel/v1", "fleetpanel/v1/", "/fleetpanel/v1/"]
)
def test_base_topic_slashes_are_normalised(base: str) -> None:
    assert build_topics(base, "abc")["meta"] == "fleetpanel/v1/devices/abc/meta"


def test_custom_base_topic() -> None:
    assert build_topics("home/panel", "abc")["telemetry"] == "home/panel/devices/abc/telemetry"


def test_parse_device_topic_round_trip() -> None:
    for leaf in ("meta", "telemetry", "availability"):
        topic = build_topics("fleetpanel/v1", "f6a3f749c2dd")[leaf]
        assert parse_device_topic("fleetpanel/v1", topic) == ("f6a3f749c2dd", leaf)


@pytest.mark.parametrize(
    "topic",
    [
        "other/v1/devices/abc/telemetry",
        "fleetpanel/v1/devices/abc",
        "fleetpanel/v1/devices//telemetry",
        "fleetpanel/v1/devices/abc/unknown",
        "fleetpanel/v1/devices/abc/telemetry/extra",
        "",
    ],
)
def test_parse_device_topic_rejects_bad_topics(topic: str) -> None:
    assert parse_device_topic("fleetpanel/v1", topic) is None


# ================================================================== history buffer


def test_capacity_covers_the_requested_window() -> None:
    assert capacity_for(900, 2.0) == 451
    assert capacity_for(300, 2.0) == 151
    assert capacity_for(0, 2.0) == 0
    assert capacity_for(900, 0.0) == 0


def test_capacity_is_hard_capped() -> None:
    assert capacity_for(86_400, 0.5) == MAX_SAMPLES


def test_buffer_never_exceeds_capacity() -> None:
    buffer = HistoryBuffer(10)
    for index in range(1000):
        buffer.append(float(index), {"sequence": index})
    assert len(buffer) == 10
    assert buffer.all()[0]["sequence"] == 990
    assert buffer.all()[-1]["sequence"] == 999


def test_buffer_since_filters_by_age() -> None:
    buffer = HistoryBuffer(100)
    for index in range(50):
        buffer.append(float(index), {"sequence": index})
    recent = buffer.since(now=49.0, seconds=10.0)
    assert [item["sequence"] for item in recent] == list(range(39, 50))


def test_zero_capacity_buffer_stores_nothing() -> None:
    buffer = HistoryBuffer(0)
    buffer.append(1.0, {"a": 1})
    assert len(buffer) == 0
    assert buffer.all() == []
    assert buffer.since(1.0, 100.0) == []


def test_buffer_clear() -> None:
    buffer = HistoryBuffer(5)
    buffer.append(1.0, {"a": 1})
    buffer.clear()
    assert len(buffer) == 0


# ==================================================================== capabilities


def _sample(**sections: Any) -> dict[str, Any]:
    base: dict[str, Any] = {
        "cpu": {
            "usage_percent": None,
            "temperature_c": None,
            "frequency_mhz": None,
            "load_1": None,
        },
        "memory": {"total_bytes": None, "swap_total_bytes": None},
        "storage": {"total_bytes": None},
        "network": {"rx_bytes_total": None},
        "optional": {"gpu": None, "battery": None},
        "status": {"process_count": None, "logged_in_users": None},
    }
    for key, value in sections.items():
        base[key] = {**base[key], **value}
    return base


def test_capabilities_track_available_data() -> None:
    caps = derive_capabilities(
        _sample(
            cpu={
                "usage_percent": 12.0,
                "temperature_c": 48.2,
                "frequency_mhz": 1800.0,
                "load_1": 0.4,
            },
            memory={"total_bytes": 1, "swap_total_bytes": 1},
            storage={"total_bytes": 1},
            network={"rx_bytes_total": 0},
            status={"process_count": 143, "logged_in_users": 1},
        )
    )
    assert caps == [
        "cpu", "cpu_temperature", "cpu_frequency", "load_average",
        "memory", "swap", "storage", "network", "processes", "sessions",
    ]


def test_capability_dropped_when_sensor_missing() -> None:
    caps = derive_capabilities(_sample(cpu={"usage_percent": 12.0}, memory={"total_bytes": 1}))
    assert "cpu" in caps
    assert "cpu_temperature" not in caps
    assert "swap" not in caps
    assert "storage" not in caps


def test_swapless_host_has_no_swap_capability() -> None:
    caps = derive_capabilities(_sample(memory={"total_bytes": 1, "swap_total_bytes": 0}))
    assert "memory" in caps
    assert "swap" not in caps


def test_gpu_and_battery_capabilities_appear_when_present() -> None:
    caps = derive_capabilities(
        _sample(optional={"gpu": {"name": "card0"}, "battery": {"percent": 80.0}})
    )
    assert "gpu" in caps
    assert "battery" in caps


def test_empty_sample_yields_no_capabilities() -> None:
    assert derive_capabilities({}) == []


# ======================================================================= meta doc


def test_meta_reports_token_auth_without_the_token() -> None:
    from fleetpanel_agent.simulator import FakeMachine, SimulatedSource

    config: AgentConfig = parse_config(
        {"http": {"auth_mode": "bearer", "api_token": "0123456789abcdef0123"}}
    )
    source = SimulatedSource(FakeMachine(1, seed=3), config)
    source.tick(2.0)
    meta = source.meta()
    assert meta["http"]["auth"] == "token"
    assert "0123456789abcdef0123" not in repr(meta)
