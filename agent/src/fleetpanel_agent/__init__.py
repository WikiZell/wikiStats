"""FleetPanel / WikiStats telemetry agent.

Serves the ``fleetpanel.telemetry.v1`` document over HTTP (FastAPI) and, optionally,
over MQTT. The same document is produced by both transports so consumers never need
transport-specific parsing.
"""

from __future__ import annotations

AGENT_VERSION = "0.1.0"
TELEMETRY_SCHEMA = "fleetpanel.telemetry.v1"
META_SCHEMA = "fleetpanel.meta.v1"
MDNS_SERVICE_TYPE = "_fleetpanel._tcp.local."

__all__ = [
    "AGENT_VERSION",
    "MDNS_SERVICE_TYPE",
    "META_SCHEMA",
    "TELEMETRY_SCHEMA",
]
