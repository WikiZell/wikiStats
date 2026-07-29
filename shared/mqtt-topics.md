# FleetPanel MQTT topic contract (v1)

MQTT is **optional**. Every agent must work over plain REST with no broker present. When
MQTT is enabled the payloads are byte-for-byte the same documents the REST API returns, so a
consumer never needs transport-specific parsing.

## Topic tree

Base topic is configurable (default `fleetpanel/v1`). All topics below are relative to it.

```text
fleetpanel/v1/devices/<device_id>/meta
fleetpanel/v1/devices/<device_id>/telemetry
fleetpanel/v1/devices/<device_id>/availability
```

`<device_id>` is the stable, hashed device ID from `device.id` in the telemetry document
(lowercase hex, 8–32 chars). It never contains `/`, `+` or `#`, so no escaping is required.

| Topic          | Retained | QoS                        | Payload                                                       |
| -------------- | -------- | -------------------------- | ------------------------------------------------------------- |
| `meta`         | yes      | 1                          | JSON metadata document (below)                                 |
| `telemetry`    | yes      | configurable, 0 or 1 (default 0) | The full `fleetpanel.telemetry.v1` document               |
| `availability` | yes      | 1                          | ASCII `online` or `offline` (no JSON, no quotes)               |

### Why `meta` is retained

A panel that boots after the agents can subscribe to `fleetpanel/v1/devices/+/meta` and
immediately learn about every device in the fleet without waiting a full sampling interval.
That is the MQTT half of discovery — the mDNS half is described in `docs/discovery.md`.

## `meta` payload

```json
{
  "schema": "fleetpanel.meta.v1",
  "id": "f6a3f749c2dd",
  "name": "WikiHermes",
  "hostname": "wikihermes",
  "platform": "linux",
  "agent_version": "0.1.0",
  "telemetry_schema": "fleetpanel.telemetry.v1",
  "http": {
    "enabled": true,
    "port": 8770,
    "path": "/api/v1/telemetry",
    "auth": "none",
    "addresses": ["192.168.1.50"]
  },
  "sample_interval_seconds": 2.0,
  "capabilities": ["cpu", "cpu_temperature", "memory", "swap", "storage", "network"]
}
```

`http.addresses` lets an MQTT-discovered device fall back to HTTP polling without a separate
mDNS lookup. It is advisory: the panel must tolerate an empty list.

## Last Will and Testament

The agent connects with:

- Will topic: `fleetpanel/v1/devices/<device_id>/availability`
- Will payload: `offline`
- Will QoS: 1
- Will retain: `true`

Immediately after `CONNACK` the agent publishes `online` (retained, QoS 1) to the same topic
and republishes `meta`. A broker-side disconnect therefore flips every subscriber to
`offline` without the panel needing its own timeout — although the panel *also* applies its
own `stale_s` / `offline_s` timers, because a wedged agent can keep its TCP session alive
while producing no samples.

## Subscription patterns for the panel

```text
fleetpanel/v1/devices/+/meta            # discovery + naming
fleetpanel/v1/devices/+/availability    # fast up/down
fleetpanel/v1/devices/+/telemetry       # data
```

The device ID is parsed from the topic and cross-checked against `device.id` inside the
payload. A mismatch is logged and the sample dropped: it means two agents were configured
with the same ID or a rogue publisher is present.

## Client IDs

- Agent: `fleetpanel-agent-<device_id>`
- Panel: `wikistats-<chip_id>`

Duplicate client IDs cause brokers to disconnect the older session in a loop, so IDs must be
derived from stable hardware identity, never random per-connect values.

## Reconnection

Both sides use exponential backoff with jitter: 1 s, 2 s, 4 s … capped at 60 s. MQTT failure
never affects the REST server in the agent, and never blocks the LVGL loop in the panel.

## Security notes

- Credentials are read from config, never logged. Log lines print `password=<set>` or
  `password=<unset>`.
- TLS is supported (`tls = true`); with a self-signed broker certificate supply
  `tls_ca_file` on the agent.
- Anyone with broker access can read telemetry. Use per-device broker ACLs restricted to
  `fleetpanel/v1/devices/<id>/#` if the broker is shared.
