"""Shared fixtures.

Tests must not depend on the machine running them: every collector test feeds
recorded or synthetic OS data. The only tests that touch the real host are the
smoke tests that assert a collector returns *a* schema-valid shape, and even those
tolerate all-null values.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

import pytest

from fleetpanel_agent.config import AgentConfig
from fleetpanel_agent.logging_setup import reset_throttle
from fleetpanel_agent.simulator import FakeMachine, SimulatedSource

REPO_ROOT = Path(__file__).resolve().parents[2]
SHARED_DIR = REPO_ROOT / "shared"


@pytest.fixture(autouse=True)
def _clear_log_throttle() -> None:
    """Throttling is process-global; a stale entry would hide a log a test asserts on."""
    reset_throttle()


@pytest.fixture
def default_config() -> AgentConfig:
    return AgentConfig()


@pytest.fixture
def sim_source(default_config: AgentConfig) -> SimulatedSource:
    source = SimulatedSource(FakeMachine(0, seed=42), default_config)
    source.tick(2.0)
    return source


@pytest.fixture
def sim_sample(sim_source: SimulatedSource) -> dict[str, Any]:
    sample = sim_source.latest()
    assert sample is not None
    return sample


@pytest.fixture
def simulator_args() -> argparse.Namespace:
    return argparse.Namespace(
        devices=2,
        port_start=18800,
        host="127.0.0.1",
        interval=2.0,
        history=60,
        seed=7,
        mdns=False,
        mqtt_host="",
        mqtt_port=1883,
        mqtt_username="",
        mqtt_password="",
        mqtt_base_topic="fleetpanel/v1",
        log_level="WARNING",
    )
