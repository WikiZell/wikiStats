# fleetpanel-agent

Read-only host telemetry for Linux and Raspberry Pi, served as
`fleetpanel.telemetry.v1` over HTTP and (optionally) MQTT. This is the half of
WikiStats that runs on the machines being monitored; the ESP32 panel firmware lives in
[`../firmware`](../firmware).

## What it does

* Samples CPU, memory, swap, storage, network and temperatures on a background thread
  (default every 2 s) and serves the **last completed sample** instantly. An HTTP
  request never waits for a measurement.
* Advertises itself over mDNS as `_fleetpanel._tcp.local.` so panels find it with no
  configuration.
* Optionally publishes the identical JSON to MQTT with a retained availability topic
  and a Last Will.
* Keeps a bounded in-memory history ring for `GET /api/v1/history`. No database.

## What it deliberately does not do

No command execution, no shell, no reboot/shutdown, no file access, no writes of any
kind. Every endpoint is a `GET`. A test asserts that no non-`GET` route exists.

## Install

One line, nothing to clone:

```bash
curl -fsSL https://raw.githubusercontent.com/WikiZell/wikiStats/main/agent/install.sh | sudo bash
```

The installer creates the `fleetpanel` system user, installs to
`/opt/fleetpanel-agent` with its own virtualenv, writes
`/etc/fleetpanel-agent/config.toml`, installs a hardened systemd unit, validates the
configuration before starting anything, and prints the URL. An existing config is
always backed up and never silently replaced.

It asks one question — whether to enable the systemd service so the agent restarts
automatically after a reboot. Answer it up front with `--service` or `--no-service`,
or skip every prompt with `--yes`.

From a checkout instead:

```bash
cd agent && sudo ./install.sh
```

With bearer authentication, non-interactive:

```bash
sudo ./install.sh --yes --service --token auto
```

Remove it, keeping the configuration:

```bash
sudo ./uninstall.sh
```

Remove everything including the config and the service user:

```bash
sudo ./uninstall.sh --purge
```

## Manage

```bash
sudo systemctl status fleetpanel-agent
```

```bash
sudo journalctl -u fleetpanel-agent -f
```

```bash
sudo systemctl restart fleetpanel-agent
```

## HTTP API

| Method | Path                    | Auth | Purpose                                    |
| ------ | ----------------------- | ---- | ------------------------------------------ |
| GET    | `/api/v1/health`        | no   | liveness, schema and version               |
| GET    | `/api/v1/schema`        | no   | the telemetry JSON Schema                  |
| GET    | `/api/v1/info`          | yes  | agent metadata (same as the MQTT `meta`)   |
| GET    | `/api/v1/telemetry`     | yes  | most recent sample                         |
| GET    | `/api/v1/history?seconds=300` | yes | bounded ring-buffer history           |
| GET    | `/api/v1/config/public` | yes  | non-secret configuration                   |
| GET    | `/api/v1/stream`        | yes  | server-sent events, one frame per sample   |
| GET    | `/docs`                 | no   | interactive OpenAPI docs                   |

"Auth" means "requires the token when `auth_mode` is not `none`". The default is
`none`, for a trusted LAN.

Test it:

```bash
curl -s http://127.0.0.1:8770/api/v1/health
```

```bash
curl -s http://127.0.0.1:8770/api/v1/telemetry | python3 -m json.tool
```

With a token:

```bash
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8770/api/v1/telemetry
```

## Configuration

`/etc/fleetpanel-agent/config.toml`. Every key is documented in
[`packaging/config.example.toml`](packaging/config.example.toml). An empty file is
valid and produces a working agent.

Validate a config without starting the service:

```bash
sudo -u fleetpanel /opt/fleetpanel-agent/venv/bin/fleetpanel-agent --config /etc/fleetpanel-agent/config.toml --check
```

A typo in a key name is a hard error, not a silent no-op.

## Temperatures

Sources are tried in order: `psutil.sensors_temperatures()`, then
`/sys/class/thermal`, then `vcgencmd measure_temp` on a Pi, then optionally
`sensors -j`. The single "CPU temperature" is chosen by label priority
(`coretemp`, `k10temp`, `cpu_thermal`, `soc_thermal`, `package`, `tctl`, …); within
one priority the hottest reading wins, which is the right answer for `coretemp`
where every core reports separately.

If nothing is available, `cpu.temperature_c` is `null` and the `cpu_temperature`
capability is dropped. The rest of the sample is unaffected.

> **systemd note.** The shipped unit sets `PrivateDevices=yes`, which hides
> `/dev/vcio`, so `vcgencmd` cannot run under it. The SoC temperature still arrives
> through `/sys/class/thermal`, which reports the same value. The unit documents how
> to re-enable `vcgencmd` if you specifically need it.

## Simulator

Run several fake machines to develop the panel without owning several computers:

```bash
python -m fleetpanel_agent.simulator --devices 5 --port-start 8800
```

Each fake device gets its own port, its own stable device ID and its own mDNS record.
Values move smoothly and independently so carousel and threshold behaviour is worth
looking at. Add `--mqtt-host broker.lan` to publish them over MQTT too.

This is the only place in the project that fabricates telemetry.

## Development

```bash
cd agent
python -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
pytest
ruff check .
mypy src
```

## Layout

```text
agent/
├── src/fleetpanel_agent/
│   ├── collectors/       cpu, memory, storage, network, temperature, host, optional
│   ├── schemas/          packaged copy of the telemetry JSON Schema
│   ├── api.py            FastAPI application (read-only)
│   ├── config.py         TOML config + validation
│   ├── discovery.py      mDNS advertisement
│   ├── identity.py       stable hashed device ID
│   ├── mqtt.py           optional MQTT publisher
│   ├── ringbuffer.py     bounded history
│   ├── sampler.py        background sampling thread
│   ├── security.py       token auth + trusted networks
│   ├── service.py        process wiring and shutdown
│   └── simulator.py      synthetic fleet for panel development
├── packaging/            systemd unit + example config
├── tests/
├── install.sh
└── uninstall.sh
```
