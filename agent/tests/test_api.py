"""HTTP API behaviour and authentication, driven by the simulator source."""

from __future__ import annotations

import asyncio
from typing import Any

import pytest
from fastapi.testclient import TestClient

from fleetpanel_agent import AGENT_VERSION, TELEMETRY_SCHEMA
from fleetpanel_agent.api import create_app
from fleetpanel_agent.config import AgentConfig, parse_config
from fleetpanel_agent.security import address_allowed, extract_token, parse_networks, token_matches
from fleetpanel_agent.simulator import FakeMachine, SimulatedSource

TOKEN = "0123456789abcdef0123456789abcdef"


def _client(config: AgentConfig, ticks: int = 3) -> TestClient:
    source = SimulatedSource(FakeMachine(0, seed=99), config)
    for _ in range(ticks):
        source.tick(config.agent.sample_interval)
    return TestClient(create_app(config, source))


@pytest.fixture
def open_client() -> TestClient:
    return _client(AgentConfig())


# ========================================================================= routes


def test_health_shape(open_client: TestClient) -> None:
    response = open_client.get("/api/v1/health")
    assert response.status_code == 200
    assert response.json() == {
        "status": "ok",
        "schema": TELEMETRY_SCHEMA,
        "agent_version": AGENT_VERSION,
    }


def test_telemetry_returns_the_latest_sample(open_client: TestClient) -> None:
    body = open_client.get("/api/v1/telemetry").json()
    assert body["schema"] == TELEMETRY_SCHEMA
    assert body["sequence"] >= 1
    assert set(body) >= {
        "schema", "timestamp", "sequence", "device", "status",
        "cpu", "memory", "storage", "network", "optional", "capabilities",
    }


def test_telemetry_sequence_increases(open_client: TestClient) -> None:
    first = open_client.get("/api/v1/telemetry").json()["sequence"]
    source: Any = open_client.app.state.sampler
    source.tick(2.0)
    second = open_client.get("/api/v1/telemetry").json()["sequence"]
    assert second == first + 1


def test_info_matches_mqtt_meta(open_client: TestClient) -> None:
    body = open_client.get("/api/v1/info").json()
    assert body["schema"] == "fleetpanel.meta.v1"
    assert body["telemetry_schema"] == TELEMETRY_SCHEMA
    assert body["http"]["path"] == "/api/v1/telemetry"
    assert body["http"]["auth"] == "none"


def test_schema_endpoint_serves_the_json_schema(open_client: TestClient) -> None:
    body = open_client.get("/api/v1/schema").json()
    assert body["title"] == TELEMETRY_SCHEMA
    assert "device" in body["$defs"]


def test_history_respects_the_window(open_client: TestClient) -> None:
    body = open_client.get("/api/v1/history?seconds=300").json()
    assert body["count"] == len(body["samples"]) >= 3
    assert body["capacity"] > 0


def test_history_rejects_absurd_windows(open_client: TestClient) -> None:
    assert open_client.get("/api/v1/history?seconds=0").status_code == 422
    assert open_client.get("/api/v1/history?seconds=999999999").status_code == 422


def test_history_disabled_returns_empty() -> None:
    config = AgentConfig()
    config.agent.history_seconds = 0
    body = _client(config).get("/api/v1/history?seconds=300").json()
    assert body["count"] == 0
    assert body["capacity"] == 0


def test_config_public_never_returns_secrets() -> None:
    config = parse_config(
        {
            "http": {"auth_mode": "bearer", "api_token": TOKEN},
            "mqtt": {"enabled": True, "host": "broker.lan", "password": "hunter2"},
        }
    )
    client = _client(config)
    response = client.get("/api/v1/config/public", headers={"Authorization": f"Bearer {TOKEN}"})
    assert response.status_code == 200
    assert TOKEN not in response.text
    assert "hunter2" not in response.text


def test_docs_available_by_default(open_client: TestClient) -> None:
    assert open_client.get("/docs").status_code == 200


def test_docs_can_be_disabled() -> None:
    config = AgentConfig()
    config.http.enable_docs = False
    assert _client(config).get("/docs").status_code == 404


def test_no_write_endpoints_exist(open_client: TestClient) -> None:
    """The agent is read-only by design; a POST route would be a security regression."""
    routes = open_client.app.routes
    for route in routes:
        for method in getattr(route, "methods", set()):
            assert method in {"GET", "HEAD", "OPTIONS"}, f"unexpected {method} on {route.path}"


async def test_stream_emits_the_current_sample() -> None:
    """SSE is driven at the ASGI level.

    Neither ``TestClient`` nor ``httpx.ASGITransport`` can consume an endless
    response - both buffer the body to completion - so the app is called directly
    with a ``receive`` that reports a disconnect as soon as the first frame lands.
    That also proves the endpoint actually stops on disconnect instead of leaking a
    subscriber.
    """
    config = AgentConfig()
    source = SimulatedSource(FakeMachine(0, seed=5), config)
    source.tick(2.0)
    app = create_app(config, source)

    sent: list[dict[str, Any]] = []
    client_gone = asyncio.Event()

    async def receive() -> dict[str, Any]:
        await client_gone.wait()
        return {"type": "http.disconnect"}

    async def send(message: dict[str, Any]) -> None:
        sent.append(message)
        if message["type"] == "http.response.body" and message.get("body"):
            client_gone.set()

    scope: dict[str, Any] = {
        "type": "http",
        "asgi": {"version": "3.0", "spec_version": "2.3"},
        "http_version": "1.1",
        "method": "GET",
        "scheme": "http",
        "path": "/api/v1/stream",
        "raw_path": b"/api/v1/stream",
        "query_string": b"",
        "root_path": "",
        "headers": [(b"host", b"panel")],
        "client": ("127.0.0.1", 50000),
        "server": ("panel", 80),
    }
    await asyncio.wait_for(app(scope, receive, send), timeout=15.0)

    start = sent[0]
    assert start["type"] == "http.response.start"
    assert start["status"] == 200
    content_type = dict(start["headers"])[b"content-type"]
    assert content_type.startswith(b"text/event-stream")

    body = b"".join(m.get("body", b"") for m in sent if m["type"] == "http.response.body")
    assert b"event: telemetry" in body
    assert TELEMETRY_SCHEMA.encode() in body
    # The generator's finally-block must have removed the subscriber.
    assert not source._subscribers


# ================================================================= authentication


def test_no_auth_by_default(open_client: TestClient) -> None:
    assert open_client.get("/api/v1/telemetry").status_code == 200


def test_bearer_required_when_configured() -> None:
    config = parse_config({"http": {"auth_mode": "bearer", "api_token": TOKEN}})
    client = _client(config)
    assert client.get("/api/v1/telemetry").status_code == 401
    bad = client.get("/api/v1/telemetry", headers={"Authorization": "Bearer wrong"})
    assert bad.status_code == 401
    assert client.get(
        "/api/v1/telemetry", headers={"Authorization": f"Bearer {TOKEN}"}
    ).status_code == 200


def test_health_and_schema_stay_public_under_auth() -> None:
    config = parse_config({"http": {"auth_mode": "bearer", "api_token": TOKEN}})
    client = _client(config)
    assert client.get("/api/v1/health").status_code == 200
    assert client.get("/api/v1/schema").status_code == 200


def test_query_token_rejected_in_bearer_mode() -> None:
    config = parse_config({"http": {"auth_mode": "bearer", "api_token": TOKEN}})
    client = _client(config)
    assert client.get(f"/api/v1/telemetry?token={TOKEN}").status_code == 401


def test_query_token_accepted_in_query_mode() -> None:
    config = parse_config({"http": {"auth_mode": "query", "api_token": TOKEN}})
    client = _client(config)
    assert client.get(f"/api/v1/telemetry?token={TOKEN}").status_code == 200
    assert client.get("/api/v1/telemetry?token=wrong").status_code == 401
    # The header still works in query mode.
    assert client.get(
        "/api/v1/telemetry", headers={"Authorization": f"Bearer {TOKEN}"}
    ).status_code == 200


def test_trusted_networks_block_outsiders() -> None:
    config = parse_config({"http": {"trusted_networks": ["10.0.0.0/8"]}})
    client = _client(config)
    # TestClient reports the client address as "testclient", which is not in any CIDR.
    assert client.get("/api/v1/telemetry").status_code == 403


def test_trusted_networks_empty_allows_everyone(open_client: TestClient) -> None:
    assert open_client.get("/api/v1/telemetry").status_code == 200


# ============================================================ security unit tests


def test_auth_dependency_request_annotation_resolves() -> None:
    """Regression, found on Debian 11 / Python 3.9.

    The dependency used to be an ``Authenticator`` instance. FastAPI resolves a
    dependency's annotations against the callable's ``__globals__``, an instance has
    none, and with ``from __future__ import annotations`` the annotation is the
    string ``"Request"``. Unresolved, FastAPI treated it as a query parameter and
    every protected route answered ``422 {"loc": ["query", "request"]}``.

    Checking the resolved signature catches this on any interpreter, rather than
    only on the ones where it happens to break.
    """
    from fastapi import Request as FastapiRequest
    from fastapi.dependencies.utils import get_typed_signature

    from fleetpanel_agent.security import auth_dependency

    parameters = get_typed_signature(auth_dependency(AgentConfig())).parameters
    assert list(parameters) == ["request"]
    assert parameters["request"].annotation is FastapiRequest, (
        "FastAPI could not resolve the Request annotation; protected routes will 422"
    )


def test_protected_routes_do_not_answer_422(open_client: TestClient) -> None:
    """422 on a route with no parameters means dependency injection went wrong."""
    for path in ("/api/v1/telemetry", "/api/v1/info", "/api/v1/config/public",
                 "/api/v1/history?seconds=60"):
        response = open_client.get(path)
        assert response.status_code != 422, f"{path} -> 422 {response.text}"
        assert response.status_code == 200, f"{path} -> {response.status_code}"


def test_address_allowed_matrix() -> None:
    networks = parse_networks(["192.168.0.0/16", "10.0.0.0/8"])
    assert address_allowed("192.168.1.50", networks) is True
    assert address_allowed("10.4.4.4", networks) is True
    assert address_allowed("172.16.0.1", networks) is False
    assert address_allowed(None, networks) is False
    assert address_allowed("not-an-ip", networks) is False
    assert address_allowed("8.8.8.8", []) is True


def test_extract_token_from_header() -> None:
    assert extract_token("Bearer abc", None, False) == "abc"
    assert extract_token("bearer abc", None, False) == "abc"
    assert extract_token("Basic abc", None, False) is None
    assert extract_token(None, "abc", False) is None
    assert extract_token(None, "abc", True) == "abc"
    assert extract_token("Bearer   ", "query", True) == "query"


def test_token_matches_is_exact() -> None:
    assert token_matches("secret", "secret") is True
    assert token_matches("secret", "Secret") is False
    assert token_matches("secret", "secre") is False
    assert token_matches("secret", None) is False
    assert token_matches("secret", "") is False
