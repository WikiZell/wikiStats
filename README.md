# WikiStats

**A desk panel that shows what your machines are doing.**

WikiStats (project name *FleetPanel*) monitors Linux and Raspberry Pi computers —
and, once someone writes the agent, Windows ones — on a 320 × 240 ESP32 touchscreen.
One page per machine, swipe between them, or let it rotate on its own.

No cloud. No broker required. No database. No Home Assistant. Nothing on your
network that can run a command on a monitored machine.

```mermaid
flowchart LR
    subgraph Machines["Your machines"]
        P["Raspberry Pi"]
        S["Debian server"]
        W["Windows PC<br/><i>future</i>"]
    end
    B[("MQTT broker<br/><i>optional</i>")]
    C["ESP32 Cheap Yellow Display"]
    Br["Browser"]

    P -->|HTTP| C
    S -->|HTTP| C
    W -.->|"same JSON"| C
    P --> B
    S --> B
    B -->|subscribe| C
    Br -->|"http://wikistats-XXXX.local"| C
```

## What you get

![The dashboard on a real ESP32-2432S028R](docs/images/dashboard.png)

*Captured from the device itself with `tools/screenshot.py`. CPU is over its
critical threshold and the temperature over its warning threshold — note that both
carry a text tag as well as a colour.*

**On the panel** — CPU percentage and temperature, RAM bar with used/total, storage
bar with used/free/total, download and upload rates, uptime, how old the reading is,
and small CPU and RAM sparklines. Warning and critical states are shown with a text
tag as well as a colour, so they survive a glossy resistive overlay and colour-blind
eyes alike. Swipe or use the arrows at the bottom to change machine.

**On any browser** — a configuration website served from the panel itself. Devices,
Wi-Fi, MQTT, thresholds, carousel, brightness, backup, firmware update. It works
with no internet access; nothing is loaded from a CDN.

**On each machine** — a small read-only Python service. It measures, it serves JSON,
and that is all it can do.

## Supported hardware

| | |
| --- | --- |
| Panel | **ESP32-2432S028R** ("Cheap Yellow Display") — 320 × 240 ILI9341, XPT2046 resistive touch, ESP32-WROOM-32, 4 MB flash, **no PSRAM** |
| Other CYD variants | supported by adding one file behind `src/hal/display_hal.h` |
| Agents | **Python 3.9+** — Debian 11+, Ubuntu 20.04+, Raspberry Pi OS bullseye+. The installer picks the newest interpreter it finds and adds the `tomli` backport on 3.9/3.10. |

## Architecture in one paragraph

Agents publish one versioned JSON document, `fleetpanel.telemetry.v1`, over HTTP
and optionally MQTT. The panel discovers them over mDNS, retained MQTT metadata, or
manual entry; merges duplicates by a stable hashed device ID; and renders them. The
document format is a shared artefact with a formal JSON Schema, and the agent ships
a byte-identical copy that a test compares against the canonical one — which is what
lets a future Windows agent work with firmware built today.

Details: [`docs/architecture.md`](docs/architecture.md).

## Installing the server (agent)

Run this on **every machine you want to watch** — Raspberry Pi, Debian, Ubuntu.
One line, nothing to clone:

```bash
curl -fsSL https://raw.githubusercontent.com/WikiZell/wikiStats/main/agent/install.sh | sudo bash
```

Prefer to read it before running it as root — always a reasonable instinct:

```bash
curl -fsSL -o wikistats-install.sh https://raw.githubusercontent.com/WikiZell/wikiStats/main/agent/install.sh
```

```bash
less wikistats-install.sh && sudo bash wikistats-install.sh
```

### What it does, and what it asks

It creates a `fleetpanel` system user, installs to `/opt/fleetpanel-agent` in its
own virtualenv, writes `/etc/fleetpanel-agent/config.toml`, installs a hardened
systemd unit, validates the configuration, and prints the URL.

It asks you one question:

```text
Start WikiStats automatically at boot? [Y/n]
```

**Yes** enables the systemd service, so the agent comes back on its own after a
reboot, a power cut or a crash. **No** installs everything but leaves it dormant;
you can change your mind at any time with `sudo systemctl enable --now fleetpanel-agent`.

If an existing `config.toml` is found it is backed up with a timestamp and you are
asked before it is replaced. Nothing is overwritten silently.

### Non-interactive

For Ansible, cloud-init or a fleet of identical machines — no prompts, sensible
answers assumed:

```bash
curl -fsSL https://raw.githubusercontent.com/WikiZell/wikiStats/main/agent/install.sh | sudo bash -s -- --yes
```

| Flag | Effect |
| ---- | ------ |
| `--yes` | never prompt; keep any existing config; enable the service |
| `--service` / `--no-service` | answer the boot question up front |
| `--no-start` | enable for next boot, but do not start now |
| `--port 9000` | HTTP port for a fresh config |
| `--token auto` | turn on bearer authentication with a generated token |
| `--mqtt-host broker.lan` | enable MQTT publishing |

Example — a locked-down agent that starts at boot, with a token:

```bash
curl -fsSL https://raw.githubusercontent.com/WikiZell/wikiStats/main/agent/install.sh | sudo bash -s -- --yes --service --token auto
```

### Check it

```bash
curl -s http://127.0.0.1:8770/api/v1/health
```

```json
{ "status": "ok", "schema": "fleetpanel.telemetry.v1", "agent_version": "0.1.0" }
```

### Manage it

```bash
sudo systemctl status fleetpanel-agent
```

```bash
sudo journalctl -u fleetpanel-agent -f
```

```bash
sudo systemctl restart fleetpanel-agent
```

Enable or disable automatic start after the fact:

```bash
sudo systemctl enable --now fleetpanel-agent
```

```bash
sudo systemctl disable --now fleetpanel-agent
```

### Remove it

Keeps `/etc/fleetpanel-agent` so a reinstall picks up where you left off:

```bash
curl -fsSL https://raw.githubusercontent.com/WikiZell/wikiStats/main/agent/uninstall.sh | sudo bash
```

Remove the configuration and the service user as well:

```bash
sudo /opt/fleetpanel-agent/uninstall.sh --purge
```

Full walkthrough, with a Raspberry Pi and a Debian example:
[`docs/linux-installation.md`](docs/linux-installation.md).

## Installing the firmware (panel)

Needs [PlatformIO](https://platformio.org/install/cli) and Node 18+ on the machine
with the USB cable, not on the panel.

```bash
git clone https://github.com/WikiZell/wikiStats.git && cd wikiStats
```

Build the configuration website first — it becomes the panel's filesystem image:

```bash
cd web && npm install && npm run build && cd ..
```

Build the firmware:

```bash
cd firmware && pio run -e cyd
```

Flash the filesystem, then the firmware. **Both are needed on a new board**; the
filesystem holds the web interface and skipping it gives you a blank browser page:

```bash
pio run -e cyd -t uploadfs
```

```bash
pio run -e cyd -t upload
```

Watch it come up:

```bash
pio device monitor -b 115200
```

On Windows add `--upload-port COM6`; on Linux the board is usually `/dev/ttyUSB0`
and PlatformIO finds it by itself. If no port appears at all, install the **CH340**
USB-serial driver.

After the first USB flash you never need the cable again — see
[updates over the air](#debugging-a-panel-that-is-already-on-the-wall).

Full walkthrough: [`docs/esp32-installation.md`](docs/esp32-installation.md).

## First boot

With no saved network the panel opens an access point called `FleetPanel-XXXX` and
shows its name and address on screen.

```mermaid
flowchart TD
    A["Power on"] --> B{"Saved network?"}
    B -->|no| C["AP FleetPanel-XXXX<br/>captive portal 192.168.4.1"]
    B -->|yes| D["Connect in priority order"]
    D -->|ok| E["Dashboard"]
    D -->|repeated failure| C
    C --> F["Set Wi-Fi from a phone<br/>or on the touchscreen"]
    F --> D
```

Join it from a phone, set your Wi-Fi, then set an administrator password. Or do the
whole thing on the touchscreen: gear → Wi-Fi.

## Adding machines

Agents advertising `_fleetpanel._tcp.local.` appear under **Discovered devices** —
approve them, or switch on automatic adding. On a network where multicast is
blocked, add them by address: Devices → *Add a device manually*.

## Using it

| Gesture | Result |
| ------- | ------ |
| Swipe left / right | previous / next machine |
| Tap the device name | device information |
| Tap a metric card | detail dialog for that metric |
| Tap the gear | settings |
| Long-press the page dots | device list |
| Any touch | pauses the carousel for 30 s |

A swipe needs at least 45 px of horizontal travel and must be mostly horizontal, so
scrolling never flips the page by accident.

## REST mode and MQTT mode

**REST** is the default and needs nothing extra. The panel polls each device every
`poll_interval_ms` (default 3 s), with per-device backoff when one stops answering.

**MQTT** is optional. Enable it on the agent and on the panel and telemetry arrives
by push instead:

```text
fleetpanel/v1/devices/<device_id>/meta           retained, QoS 1
fleetpanel/v1/devices/<device_id>/telemetry      retained, QoS 0 or 1
fleetpanel/v1/devices/<device_id>/availability   retained, QoS 1, "online"/"offline"
```

`availability` is backed by a Last Will, so a crashed agent flips to `offline` at the
broker without waiting for a timeout. Full contract:
[`shared/mqtt-topics.md`](shared/mqtt-topics.md).

**Automatic** mode prefers MQTT while it is connected *and delivering recent
samples*, and falls back to HTTP otherwise. Connection alone is not enough — a
broker that is up with no publisher looks healthy and would leave every tile blank.

## The telemetry document

```json
{
  "schema": "fleetpanel.telemetry.v1",
  "timestamp": "2026-07-28T20:45:30Z",
  "sequence": 1234,
  "device": {
    "id": "f6a3f749c2dd", "name": "WikiHermes", "hostname": "wikihermes",
    "platform": "linux", "os_name": "Debian GNU/Linux 12", "os_version": "12",
    "kernel": "6.6.31+rpt-rpi-v8", "architecture": "aarch64",
    "hardware_model": "Raspberry Pi 4 Model B Rev 1.5", "agent_version": "0.1.0"
  },
  "status": { "state": "online", "uptime_seconds": 348122, "boot_time": "2026-07-24T20:03:28Z",
              "process_count": 143, "logged_in_users": 1 },
  "cpu": { "usage_percent": 17.4, "per_core_percent": [14.0, 22.1, 13.6, 19.8],
           "physical_cores": 4, "logical_cores": 4, "frequency_mhz": 1800.0,
           "load_1": 0.41, "load_5": 0.30, "load_15": 0.27, "temperature_c": 48.2,
           "temperatures": [{ "label": "cpu", "temperature_c": 48.2, "high_c": 80.0, "critical_c": 90.0 }] },
  "memory": { "total_bytes": 8589934592, "available_bytes": 5368709120, "used_bytes": 3221225472,
              "free_bytes": 4294967296, "usage_percent": 37.5, "swap_total_bytes": 1073741824,
              "swap_used_bytes": 134217728, "swap_free_bytes": 939524096, "swap_usage_percent": 12.5 },
  "storage": { "total_bytes": 62538170368, "used_bytes": 27692531712, "free_bytes": 34845638656,
               "usage_percent": 44.3, "mounts": [ /* … */ ] },
  "network": { "primary_interface": "eth0", "ip_addresses": ["192.168.1.50"],
               "rx_bytes_total": 28478212231, "tx_bytes_total": 9876241132,
               "rx_bytes_per_second": 24563.2, "tx_bytes_per_second": 4311.7 },
  "optional": { "gpu": null, "battery": null },
  "capabilities": ["cpu", "cpu_temperature", "memory", "swap", "storage", "network"]
}
```

Bytes for sizes, bytes per second for rates, Celsius, MHz, 0–100 percentages,
seconds for uptime, UTC ISO 8601 timestamps. Anything the host cannot measure is
`null` and drops out of `capabilities` — never omitted, never faked.

Field-by-field: [`docs/protocol.md`](docs/protocol.md).
Formal schema: [`shared/telemetry-v1.schema.json`](shared/telemetry-v1.schema.json).

## Agent HTTP API

| Method | Path | Auth | Purpose |
| ------ | ---- | ---- | ------- |
| GET | `/api/v1/health` | no | liveness |
| GET | `/api/v1/schema` | no | the JSON Schema |
| GET | `/api/v1/info` | yes | agent metadata |
| GET | `/api/v1/telemetry` | yes | latest sample |
| GET | `/api/v1/history?seconds=300` | yes | bounded ring buffer |
| GET | `/api/v1/config/public` | yes | non-secret configuration |
| GET | `/api/v1/stream` | yes | server-sent events |
| GET | `/docs` | no | OpenAPI UI |

Every route is a `GET`. There is no command execution, no shell, no reboot and no
file access anywhere in the API, and a test enumerates the route table to keep it
that way.

## Testing without five computers

```bash
python -m fleetpanel_agent.simulator --devices 5 --port-start 8800
```

Five synthetic machines on five ports, each with its own stable ID and mDNS record,
with values that move smoothly and independently. This is the only place in the
project that fabricates telemetry.

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

```bash
cd firmware
pio run
pio test -e native
pio run -t upload
pio run -t uploadfs
pio device monitor
```

```bash
cd web
npm install
npm run build
```

## Debugging a panel that is already on the wall

```bash
nc wikistats-XXXX.local 23
```

Every log line also goes to TCP port 23 and to `GET /api/logs`. It is output-only:
anything typed at it is discarded, so it can never become a command channel.

You can also see what is on the screen without being in the room:

```bash
python tools/screenshot.py wikistats-XXXX.local dashboard.png
```

Firmware updates over the air (the password goes in an environment variable —
`--upload-flags` is not a `pio run` option):

```bash
PLATFORMIO_UPLOAD_FLAGS=--auth=YOURPASSWORD pio run -e cyd-ota -t upload --upload-port wikistats-XXXX.local
```

Or upload `firmware.bin` from the web interface's *Firmware update* page, which
needs no inbound firewall rule.

## Security, honestly

Designed for a **trusted LAN**. Telemetry and the panel's admin password cross your
network in clear text; HTTPS is not offered because certificate management on a
clockless ESP32 is worse than the problem it solves. The setup portal is briefly an
open access point. MQTT TLS on the panel encrypts but does not verify the broker.

What *is* guaranteed: the agent cannot be made to run anything, credentials are
never logged or returned by a read endpoint, the admin password is PBKDF2-hashed,
mutating requests need a CSRF token, and login attempts are rate limited.

The full list of limitations, stated plainly:
[`docs/security.md`](docs/security.md).

## Troubleshooting

Common failures and what they mean:
[`docs/troubleshooting.md`](docs/troubleshooting.md).

## Future Windows support

The protocol was designed for this from the start, so a Windows agent needs **no
firmware change and no panel setting**. It has to emit the same document with
`platform: "windows"` and `null` for what it cannot measure (CPU temperature usually
needs WMI or a vendor driver, so `cpu.temperature_c` will often be `null` — the
panel already hides the tile and drops the capability).

There is already a firmware test that parses a Windows-shaped document to prove the
parser needs nothing new. Suggested route:

1. Reuse `fleetpanel_agent` unchanged for the API, config, MQTT and discovery layers
   — they are platform-neutral.
2. Add `collectors/windows.py` for the parts psutil cannot do: `hardware_model` from
   `Win32_ComputerSystem`, `os_name`/`os_version` from `Win32_OperatingSystem`,
   temperatures from `MSAcpi_ThermalZoneTemperature` where the vendor exposes it.
3. Set `platform: "windows"` in `HostCollector._platform_family` — already
   implemented.
4. Package with a service wrapper instead of systemd.
5. Validate against `shared/telemetry-v1.schema.json` in CI, exactly as the Linux
   agent does.

## Repository layout

```text
wikistats/
├── agent/            Python telemetry agent, installer, systemd unit, tests
├── firmware/         PlatformIO ESP32 firmware
│   ├── lib/fleet_core/   platform-independent, host-testable core
│   ├── src/hal/          display and touch abstraction
│   ├── src/net/          transports, discovery, web server, OTA
│   ├── src/ui/           LVGL screens
│   └── test/             native unit tests
├── web/              Vite + TypeScript configuration website
├── shared/           JSON Schemas, example document, MQTT contract
├── docs/             architecture, protocol, installation, discovery, security
└── .github/workflows/
```

## Licence

MIT — see [`LICENSE`](LICENSE). Dependency licences, including the two LGPL
libraries and what that implies, are listed in
[`docs/architecture.md`](docs/architecture.md#licences).
