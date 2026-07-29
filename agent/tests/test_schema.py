"""JSON Schema conformance.

Three things are checked:

1. The packaged schema is byte-identical to ``shared/telemetry-v1.schema.json``, so
   the copy shipped inside the wheel can never drift from the contract the firmware
   was written against.
2. The documented example validates.
3. Live simulator output validates - this is the check that catches a collector
   emitting a wrong type or dropping a required key.
"""

from __future__ import annotations

import json
from typing import Any

import jsonschema
import pytest

from fleetpanel_agent.schema import TELEMETRY_SCHEMA_PATH, telemetry_schema
from fleetpanel_agent.simulator import FakeMachine

from .conftest import SHARED_DIR


def _validator() -> jsonschema.protocols.Validator:
    schema = telemetry_schema()
    cls = jsonschema.validators.validator_for(schema)
    cls.check_schema(schema)
    return cls(schema)


def test_packaged_schema_matches_shared_copy() -> None:
    shared = (SHARED_DIR / "telemetry-v1.schema.json").read_text(encoding="utf-8")
    packaged = TELEMETRY_SCHEMA_PATH.read_text(encoding="utf-8")
    assert shared == packaged, (
        "agent/src/fleetpanel_agent/schemas/telemetry-v1.schema.json has drifted from "
        "shared/telemetry-v1.schema.json; copy the shared file over it"
    )


def test_schema_is_itself_valid() -> None:
    _validator()  # check_schema raises if the schema document is malformed


def test_documented_example_validates() -> None:
    example = json.loads((SHARED_DIR / "telemetry-example-linux.json").read_text(encoding="utf-8"))
    _validator().validate(example)


def test_simulator_output_validates(sim_sample: dict[str, Any]) -> None:
    _validator().validate(sim_sample)


@pytest.mark.parametrize("index", range(4))
def test_every_simulator_profile_validates(index: int) -> None:
    machine = FakeMachine(index, seed=index)
    validator = _validator()
    for _ in range(5):
        validator.validate(machine.tick(2.0))


def test_configuration_schema_is_valid() -> None:
    schema = json.loads((SHARED_DIR / "configuration-v1.schema.json").read_text(encoding="utf-8"))
    jsonschema.validators.validator_for(schema).check_schema(schema)


def test_unknown_fields_are_accepted() -> None:
    """Forward compatibility: a v1.1 agent may add keys a v1.0 consumer ignores."""
    example = json.loads((SHARED_DIR / "telemetry-example-linux.json").read_text(encoding="utf-8"))
    example["future_section"] = {"anything": 1}
    example["cpu"]["future_field"] = "value"
    example["device"]["cpu_vendor"] = "ACME"
    _validator().validate(example)


def test_null_values_are_accepted_for_unsupported_metrics() -> None:
    """A host with no sensors still produces a valid document."""
    example = json.loads((SHARED_DIR / "telemetry-example-linux.json").read_text(encoding="utf-8"))
    example["cpu"]["temperature_c"] = None
    example["cpu"]["temperatures"] = None
    example["cpu"]["frequency_mhz"] = None
    example["memory"]["swap_total_bytes"] = None
    example["storage"]["mounts"] = None
    example["network"]["ip_addresses"] = None
    example["capabilities"] = ["cpu", "memory"]
    _validator().validate(example)


def test_missing_required_section_is_rejected() -> None:
    example = json.loads((SHARED_DIR / "telemetry-example-linux.json").read_text(encoding="utf-8"))
    del example["cpu"]
    with pytest.raises(jsonschema.ValidationError):
        _validator().validate(example)


def test_percentage_out_of_range_is_rejected() -> None:
    example = json.loads((SHARED_DIR / "telemetry-example-linux.json").read_text(encoding="utf-8"))
    example["cpu"]["usage_percent"] = 140.0
    with pytest.raises(jsonschema.ValidationError):
        _validator().validate(example)


def test_wrong_schema_discriminator_is_rejected() -> None:
    example = json.loads((SHARED_DIR / "telemetry-example-linux.json").read_text(encoding="utf-8"))
    example["schema"] = "something.else.v1"
    with pytest.raises(jsonschema.ValidationError):
        _validator().validate(example)


def test_timestamp_must_be_utc_iso8601() -> None:
    example = json.loads((SHARED_DIR / "telemetry-example-linux.json").read_text(encoding="utf-8"))
    example["timestamp"] = "2026-07-28 20:45:30"
    with pytest.raises(jsonschema.ValidationError):
        _validator().validate(example)
