"""Access to the packaged copy of the telemetry JSON Schema.

The canonical document lives in ``shared/telemetry-v1.schema.json`` at the repository
root; an identical copy ships inside the package so an installed agent can serve
``GET /api/v1/schema`` without the repository present. ``tests/test_schema.py``
asserts the two files are byte-identical, which makes drift a CI failure rather than
a support ticket.
"""

from __future__ import annotations

import json
from functools import lru_cache
from pathlib import Path
from typing import Any

SCHEMA_DIR = Path(__file__).parent / "schemas"
TELEMETRY_SCHEMA_PATH = SCHEMA_DIR / "telemetry-v1.schema.json"


@lru_cache(maxsize=1)
def telemetry_schema() -> dict[str, Any]:
    data: dict[str, Any] = json.loads(TELEMETRY_SCHEMA_PATH.read_text(encoding="utf-8"))
    return data


def telemetry_schema_text() -> str:
    return TELEMETRY_SCHEMA_PATH.read_text(encoding="utf-8")
