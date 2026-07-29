# Installing the agent on Linux

Two worked examples: a Raspberry Pi and a generic Debian/Ubuntu machine. The
installer is the same; only the details differ.

## Requirements

* Python 3.11 or newer (`apt install python3 python3-venv` on bookworm and later)
* systemd
* Debian 12+, Ubuntu 22.04+, or Raspberry Pi OS bookworm+ (anything else works but
  is untested and the installer says so)

## Example 1 — Raspberry Pi 4 running Raspberry Pi OS

```bash
sudo apt update && sudo apt install -y git python3-venv
git clone https://github.com/fleetpanel/wikistats.git
cd wikistats/agent
sudo ./install.sh
```

Output ends with something like:

```text
FleetPanel agent installed.

  Device ID     f6a3f749c2dd
  Telemetry     http://192.168.1.50:8770/api/v1/telemetry
  Health        http://192.168.1.50:8770/api/v1/health
  API docs      http://192.168.1.50:8770/docs
  mDNS          _fleetpanel._tcp.local.
  Config        /etc/fleetpanel-agent/config.toml
```

Verify:

```bash
curl -s http://127.0.0.1:8770/api/v1/health
```

```json
{ "status": "ok", "schema": "fleetpanel.telemetry.v1", "agent_version": "0.1.0" }
```

Check the Pi-specific fields came through:

```bash
curl -s http://127.0.0.1:8770/api/v1/telemetry | python3 -m json.tool | grep -E "hardware_model|temperature_c"
```

```text
"hardware_model": "Raspberry Pi 4 Model B Rev 1.5",
"temperature_c": 48.2,
```

`hardware_model` comes from `/proc/device-tree/model`. The temperature comes from
`/sys/class/thermal/thermal_zone0` — see the note about `vcgencmd` below.

### Raspberry Pi temperature and `vcgencmd`

The shipped systemd unit sets `PrivateDevices=yes`, which hides `/dev/vcio`, so
`vcgencmd` cannot run under it. This is deliberate: `/sys/class/thermal` reports the
same SoC temperature and needs no device access at all.

If you specifically want `vcgencmd`, add to the unit:

```ini
PrivateDevices=no
DeviceAllow=/dev/vcio rw
SupplementaryGroups=video
```

## Example 2 — Debian or Ubuntu server, with a token

A machine reachable from more than one network should not answer everything:

```bash
cd wikistats/agent
sudo ./install.sh --token auto --port 8770
```

The generated token is printed once. Then restrict who may ask at all:

```bash
sudo nano /etc/fleetpanel-agent/config.toml
```

```toml
[http]
auth_mode = "bearer"
api_token = "…"
trusted_networks = ["192.168.0.0/16", "10.0.0.0/8"]
```

```bash
sudo systemctl restart fleetpanel-agent
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8770/api/v1/telemetry | head -c 200
```

## Enabling MQTT

Optional. REST keeps working with no broker.

```toml
[mqtt]
enabled = true
host = "broker.lan"
port = 1883
username = "fleetpanel"
password = "…"
base_topic = "fleetpanel/v1"
telemetry_qos = 0
telemetry_retain = true
```

```bash
sudo systemctl restart fleetpanel-agent
mosquitto_sub -h broker.lan -t 'fleetpanel/v1/devices/+/availability' -v
```

```text
fleetpanel/v1/devices/f6a3f749c2dd/availability online
```

The password is never written to the log. Log lines report `password=<set>`.

## Managing the service

```bash
sudo systemctl status fleetpanel-agent
```

```bash
sudo journalctl -u fleetpanel-agent -f
```

```bash
sudo systemctl restart fleetpanel-agent
```

Validate a configuration change without restarting anything:

```bash
sudo -u fleetpanel /opt/fleetpanel-agent/venv/bin/fleetpanel-agent --config /etc/fleetpanel-agent/config.toml --check
```

A typo in a key name is a hard error with the offending key named. There is no
silent fallback — a setting that does not apply must be visible.

## What the installer does

| Step | Detail |
| ---- | ------ |
| Preflight | root, systemd, Python ≥ 3.11, `venv` module, correct working directory |
| User | `fleetpanel` system user, no shell, no home |
| Application | `/opt/fleetpanel-agent`, owned by root, group-read only |
| Virtualenv | `/opt/fleetpanel-agent/venv`, dependencies from `pyproject.toml` |
| Config | `/etc/fleetpanel-agent/config.toml`, mode 0640, root:fleetpanel |
| Validation | runs `--check` and **aborts before starting** if it fails |
| Service | hardened unit, enabled and started |
| Summary | device ID, URLs, management commands |

An existing configuration is always copied to
`config.toml.<timestamp>.bak` first, and is only replaced after an explicit
confirmation (`--yes` keeps it).

## Uninstalling

Keep the configuration:

```bash
sudo ./uninstall.sh
```

Remove everything including credentials and the service user:

```bash
sudo ./uninstall.sh --purge
```

## Running without installing

Useful while developing:

```bash
cd agent
python -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
python -m fleetpanel_agent --config /dev/null --port 8770
```

## Firewall

The agent needs one inbound TCP port (default 8770) and, for discovery, UDP 5353:

```bash
sudo ufw allow 8770/tcp
sudo ufw allow 5353/udp
```

Without 5353 the agent still works; the panel just has to be pointed at it manually.
