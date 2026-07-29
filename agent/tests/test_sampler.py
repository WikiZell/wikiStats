"""Sampler failure handling, non-blocking reads and clean lifecycle.

These run against the real host collectors but assert only on structure, never on
values, so they pass on a laptop, a Pi, a container and a Windows CI runner alike.
"""

from __future__ import annotations

import time
from typing import Any

import jsonschema
import pytest

from fleetpanel_agent.config import AgentConfig, parse_config
from fleetpanel_agent.sampler import Sampler
from fleetpanel_agent.schema import telemetry_schema


@pytest.fixture
def sampler() -> Any:
    config = parse_config({"agent": {"sample_interval": 0.5, "history_seconds": 30}})
    sampler = Sampler(config, "f6a3f749c2dd", "TestHost")
    yield sampler
    sampler.stop(timeout=2.0)


def test_first_sample_validates_against_the_schema(sampler: Sampler) -> None:
    sampler._cpu.prime()
    sample = sampler.sample_once()
    jsonschema.validate(sample, telemetry_schema())


def test_sequence_is_monotonic(sampler: Sampler) -> None:
    first = sampler.sample_once()["sequence"]
    second = sampler.sample_once()["sequence"]
    third = sampler.sample_once()["sequence"]
    assert [second - first, third - second] == [1, 1]


def test_identity_is_carried_into_every_sample(sampler: Sampler) -> None:
    sample = sampler.sample_once()
    assert sample["device"]["id"] == "f6a3f749c2dd"
    assert sample["device"]["name"] == "TestHost"
    assert sample["device"]["agent_version"]


def test_a_failing_collector_degrades_instead_of_raising(
    sampler: Sampler, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(
        sampler._memory, "collect", lambda: (_ for _ in ()).throw(OSError("meminfo gone"))
    )
    sample = sampler.sample_once()
    assert sample["status"]["state"] == "degraded"
    assert sample["memory"]["total_bytes"] is None
    assert "memory" not in sample["capabilities"]
    # Everything else still collected.
    assert sample["cpu"]["logical_cores"] is not None
    jsonschema.validate(sample, telemetry_schema())


def test_every_collector_failing_still_produces_a_valid_document(
    sampler: Sampler, monkeypatch: pytest.MonkeyPatch
) -> None:
    def boom(*_a: object, **_k: object) -> Any:
        raise OSError("total sensor failure")

    for component in ("_cpu", "_memory", "_storage", "_network", "_optional"):
        monkeypatch.setattr(getattr(sampler, component), "collect", boom)
    sample = sampler.sample_once()
    assert sample["status"]["state"] == "degraded"
    assert sample["capabilities"] == [] or set(sample["capabilities"]) <= {"processes", "sessions"}
    jsonschema.validate(sample, telemetry_schema())


def test_latest_is_available_without_blocking(sampler: Sampler) -> None:
    assert sampler.latest() is None
    sampler.sample_once()
    started = time.monotonic()
    for _ in range(2000):
        assert sampler.latest() is not None
    # 2000 reads of a cached dict; if this ever measures CPU it would take ~30 minutes.
    assert time.monotonic() - started < 1.0


def test_history_is_bounded_by_configuration() -> None:
    config = parse_config({"agent": {"sample_interval": 1.0, "history_seconds": 5}})
    sampler = Sampler(config, "abc", "T")
    try:
        for _ in range(50):
            sampler.sample_once()
        assert sampler.history_capacity() == 6
        assert len(sampler.history(3600)) <= 6
    finally:
        sampler.stop(timeout=1.0)


def test_history_disabled_returns_nothing() -> None:
    config = parse_config({"agent": {"history_seconds": 0}})
    sampler = Sampler(config, "abc", "T")
    try:
        sampler.sample_once()
        assert sampler.history(300) == []
        assert sampler.history_capacity() == 0
    finally:
        sampler.stop(timeout=1.0)


def test_publisher_failure_does_not_stop_sampling(sampler: Sampler) -> None:
    calls: list[int] = []

    def broken(_sample: dict[str, Any]) -> None:
        calls.append(1)
        raise RuntimeError("broker exploded")

    sampler.add_publisher(broken)
    sampler.sample_once()
    sampler.sample_once()
    assert len(calls) == 2  # called both times, both failures swallowed


def test_subscribers_receive_samples_and_are_bounded(sampler: Sampler) -> None:
    queue = sampler.subscribe()
    try:
        for _ in range(20):
            sampler.sample_once()
        assert queue.qsize() <= 4  # bounded: a stalled client cannot grow the heap
        assert queue.get_nowait()["schema"] == "fleetpanel.telemetry.v1"
    finally:
        sampler.unsubscribe(queue)
    assert sampler.subscriber_count() == 0


def test_start_then_stop_is_clean() -> None:
    config = parse_config({"agent": {"sample_interval": 0.5, "history_seconds": 10}})
    sampler = Sampler(config, "abc", "T")
    sampler.start()
    assert sampler.latest() is not None  # start() takes one sample synchronously
    time.sleep(1.2)
    assert sampler.latest()["sequence"] >= 2
    sampler.stop(timeout=2.0)
    sequence_after_stop = sampler.latest()["sequence"]
    time.sleep(0.8)
    assert sampler.latest()["sequence"] == sequence_after_stop


def test_meta_document_shape(sampler: Sampler) -> None:
    sampler.sample_once()
    meta = sampler.meta()
    assert meta["schema"] == "fleetpanel.meta.v1"
    assert meta["id"] == "f6a3f749c2dd"
    assert meta["http"]["path"] == "/api/v1/telemetry"
    assert isinstance(meta["capabilities"], list)


def test_default_sample_interval_is_two_seconds() -> None:
    assert AgentConfig().agent.sample_interval == 2.0
