# Telemetry protocol — `fleetpanel.telemetry.v1`

The contract between every agent and every panel. The formal schema is
[`shared/telemetry-v1.schema.json`](../shared/telemetry-v1.schema.json); this page
explains the reasoning behind it.

## Design rules

1. **One shape for every operating system.** Field names, types, units and nesting
   are identical on Linux, Raspberry Pi OS and any future Windows or macOS agent.
   The panel firmware has no per-platform branches.
2. **`null`, never absent.** A value the host cannot produce is present and `null`.
   A missing key and a zero are both ambiguous; `null` is not.
3. **Capabilities are derived, not declared.** `capabilities` lists only the
   subsystems that produced usable data *in this sample*. A machine whose
   temperature sensor disappears loses `cpu_temperature` from the next sample.
4. **Additive changes only within v1.** New fields may appear at any time.
   Consumers must ignore unknown keys — the firmware parser does, and there is a
   test for it. A breaking change means `fleetpanel.telemetry.v2`.
5. **No units in field names, no ambiguity in values.**

| Quantity            | Unit                   | Example field            |
| ------------------- | ---------------------- | ------------------------ |
| Memory, storage     | bytes                  | `memory.total_bytes`     |
| Cumulative network  | bytes                  | `network.rx_bytes_total` |
| Network rate        | bytes per second       | `network.rx_bytes_per_second` |
| Temperature         | degrees Celsius        | `cpu.temperature_c`      |
| CPU frequency       | MHz                    | `cpu.frequency_mhz`      |
| Percentage          | 0–100                  | `cpu.usage_percent`      |
| Uptime              | seconds                | `status.uptime_seconds`  |
| Timestamps          | UTC ISO 8601, `Z`      | `timestamp`              |

## Envelope

```json
{
  "schema": "fleetpanel.telemetry.v1",
  "timestamp": "2026-07-28T20:45:30Z",
  "sequence": 1234,
  "device": { "...": "..." },
  "status": { "...": "..." },
  "cpu": { "...": "..." },
  "memory": { "...": "..." },
  "storage": { "...": "..." },
  "network": { "...": "..." },
  "optional": { "gpu": null, "battery": null },
  "capabilities": ["cpu", "memory"]
}
```

`sequence` increases monotonically from 0 at agent start. It is not persisted: a
restarted agent starts again at 1, and a consumer that sees the sequence go
backwards should treat it as "the agent restarted", not as data loss.

## `device`

| Field            | Type            | Notes |
| ---------------- | --------------- | ----- |
| `id`             | string          | 8–32 lowercase hex. Stable across reboots, IP changes and renames. |
| `name`           | string          | Display name. Defaults to the short hostname. |
| `hostname`       | string          | As the OS reports it. |
| `platform`       | enum            | `linux` \| `windows` \| `macos` \| `freebsd` \| `other` |
| `os_name`        | string \| null  | `PRETTY_NAME` on Linux. |
| `os_version`     | string \| null  | `VERSION_ID` on Linux. |
| `kernel`         | string \| null  | |
| `architecture`   | string \| null  | `aarch64`, `x86_64`, `AMD64`… |
| `hardware_model` | string \| null  | Device tree model on a Pi, DMI vendor + product on x86. |
| `agent_version`  | string          | |

### Device identity

The ID is `sha256("fleetpanel.device-id.v1" + machine_id)[:12]`.

* The raw `/etc/machine-id` is **never** published. Other software treats it as a
  semi-secret host identifier, and a fixed application salt means the published
  value cannot be correlated with IDs published by unrelated software that hashes
  the same input.
* Without a machine ID (some containers, some minimal images) the primary MAC
  address is used instead, which is stable for the life of the hardware.
* An administrator can override it in `config.toml`. Two machines sharing an ID
  will fight over one panel entry, so the override exists for migrations, not for
  convenience.

## `status`

| Field             | Type           | Notes |
| ----------------- | -------------- | ----- |
| `state`           | enum           | `online` \| `degraded`. `degraded` means at least one collector failed for this sample. |
| `uptime_seconds`  | int \| null    | |
| `boot_time`       | ISO 8601 \| null | |
| `process_count`   | int \| null    | |
| `logged_in_users` | int \| null    | **Distinct usernames**, not sessions. Three terminals is one user. |

The panel decides online/stale/offline from the **age of the last sample it
received**, never from `state`. A wedged agent keeps its TCP session and keeps
claiming `online`; sample age catches that, self-reporting does not.

## `cpu`

`temperature_c` is the single number a human would quote. It is chosen from
`temperatures[]` by documented label priority:

```text
coretemp → k10temp → cpu_thermal → soc_thermal → package → tctl →
cpu-thermal → x86_pkg_temp → cpu → (anything else)
```

Within one priority level the **hottest** reading wins, because `coretemp` reports
one entry per core and the package maximum is the number that matters for
throttling.

`temperatures[]` carries every usable sensor with its `high_c` and `critical_c`
limits where the driver supplies them. A limit reported as 0 is dropped to `null`:
several drivers use 0 to mean "not applicable".

## `memory`

`usage_percent` is derived from `available_bytes`, not from `used_bytes`. A Linux
box with 6 GiB in page cache is not 90 % full, and reporting it that way makes
every threshold useless.

## `storage`

Aggregate totals count each backing device **once**. Without that, a modern desktop
triple-counts: snaps are squashfs loop mounts, containers add overlays, and bind
mounts report the same block device twice.

Selection rules, in order:

1. An explicit `include` list, if configured, wins and disables the pseudo filter.
   `include = ["/"]` means the root filesystem, not "every absolute path".
2. Otherwise pseudo filesystem types (`tmpfs`, `overlay`, `squashfs`, `proc`,
   `sysfs`, `cgroup*`, `fuse.*`, …) and synthetic mount prefixes (`/proc`, `/sys`,
   `/dev`, `/run`, `/snap`, `/var/lib/docker`, …) are dropped.
3. Anything in `exclude` is dropped.
4. Filesystems under `min_total_bytes` (default 64 MiB) are dropped, except `/`.
5. Remaining mounts are deduplicated by backing device, preferring the shortest
   mountpoint.

## `network`

Totals aggregate every physical, non-loopback interface. Virtual interfaces
(`docker*`, `veth*`, `br-*`, `virbr*`, `tun*`, `wg*`, `zt*`, …) are excluded — their
traffic is usually the same packets counted twice, which would double every rate on
a container host.

Rates are computed from consecutive counter readings against a **monotonic** clock,
so an NTP step cannot produce a spike. A counter that goes backwards (interface
reset, 32-bit wrap, NIC replaced) yields `0.0` for that interval rather than a
negative or astronomical number.

## `optional`

Always an object; each member is `null` when unsupported.

* `gpu` — populated from `amdgpu`/`i915` sysfs counters, or `nvidia-smi` when the
  binary exists. No driver libraries are linked.
* `battery` — `psutil.sensors_battery()`.

## Forward compatibility

The firmware parser (`firmware/lib/fleet_core/fp_telemetry.cpp`) applies these rules,
each covered by a native test:

| Input                                   | Behaviour |
| --------------------------------------- | --------- |
| Unknown top-level section               | ignored |
| Unknown field inside a known section    | ignored |
| `null` where a number is expected       | treated as "no value"; the tile shows `--` |
| Wrong type (`"lots"` for a percentage)  | that one field becomes "no value"; the rest of the sample is kept |
| `schema` = `fleetpanel.telemetry.v1.4`  | accepted (prefix match) |
| `schema` = anything else                | rejected |
| Missing `device.id`                     | rejected — a sample that cannot be attributed is worse than no sample |
| Payload over the configured ceiling     | rejected **before** parsing |
| 128 cores / 40 temperature entries      | truncated to 16 / 6 |

An ArduinoJson filter means the panel only ever allocates for the fields it renders,
so a hostile or buggy agent cannot exhaust a heap that has no PSRAM behind it.

## Writing another agent

Emit this document and you are done — no firmware change, no panel setting. The
checklist:

1. `schema` exactly `fleetpanel.telemetry.v1`.
2. A stable, hashed `device.id`.
3. `platform` set to the right enum value.
4. Everything you cannot measure set to `null`, and left out of `capabilities`.
5. Serve it at `GET /api/v1/telemetry`, and/or publish it to
   `fleetpanel/v1/devices/<id>/telemetry`.
6. Advertise `_fleetpanel._tcp.local.` with the TXT records in
   [`discovery.md`](discovery.md) so panels find you automatically.

Validate against the schema before shipping:

```bash
python -c "import json,jsonschema;jsonschema.validate(json.load(open('sample.json')),json.load(open('shared/telemetry-v1.schema.json')))"
```
