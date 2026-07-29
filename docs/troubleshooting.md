# Troubleshooting

## Agent

### The service will not start

```bash
sudo systemctl status fleetpanel-agent
sudo journalctl -u fleetpanel-agent -n 50 --no-pager
```

Validate the configuration directly — a bad key is reported by name:

```bash
sudo -u fleetpanel /opt/fleetpanel-agent/venv/bin/fleetpanel-agent --config /etc/fleetpanel-agent/config.toml --check
```

| Message | Meaning |
| ------- | ------- |
| `invalid configuration … extra_forbidden` | a key is misspelled or in the wrong section |
| `http.auth_mode = 'bearer' requires a non-empty http.api_token` | token mode without a token |
| `http.api_token is set but http.auth_mode = 'none'` | you believe the agent is protected; it is not |
| `both http.enabled and mqtt.enabled are false` | the agent would do nothing |
| `cannot read …: Expected '=' after a key` | malformed TOML |

### Port already in use

```bash
sudo ss -lptn 'sport = :8770'
```

Change `http.port` and restart.

### `temperature_c` is null

Expected on VMs, most containers, and some SBCs. Check in order:

```bash
python3 -c "import psutil,json;print(json.dumps(psutil.sensors_temperatures(),default=str))"
ls /sys/class/thermal/
cat /sys/class/thermal/thermal_zone0/type /sys/class/thermal/thermal_zone0/temp
```

If sysfs has readings but the agent reports `null`, the labels do not match the
priority list — add yours to `temperature.preferred_labels`.

If both are empty, install `lm-sensors` and enable the opt-in source:

```bash
sudo apt install lm-sensors && sudo sensors-detect --auto
```

```toml
[temperature]
use_sensors_json = true
```

On a Raspberry Pi under systemd, `vcgencmd` cannot run because `PrivateDevices=yes`
hides `/dev/vcio` — `/sys/class/thermal` reports the same value. See
[linux-installation.md](linux-installation.md#raspberry-pi-temperature-and-vcgencmd).

Whatever happens, the rest of the sample is unaffected: `cpu_temperature` simply
drops out of `capabilities` and the panel hides the tile.

### Storage totals look wrong

Almost always duplicate or pseudo mounts. See what the agent kept:

```bash
curl -s http://127.0.0.1:8770/api/v1/telemetry | python3 -c "import json,sys;[print(m['mountpoint'], m['device'], m['total_bytes']) for m in json.load(sys.stdin)['storage']['mounts']]"
```

Then pin it down:

```toml
[storage]
include = ["/", "/mnt/data"]
```

`include = ["/"]` means the root filesystem, not "everything".

### Network rates are zero or absurd

* Zero on the very first sample is correct — there is no previous reading to
  subtract from.
* Permanently zero means the interface is classified as virtual. The agent
  excludes `docker*`, `veth*`, `br-*`, `virbr*`, `tun*`, `wg*`, `zt*` and friends.
* A single zero after a spike means the counter went backwards (interface reset or
  32-bit wrap) and the agent deliberately reported `0.0` rather than a negative
  number.

### High CPU usage from the agent

Lower the sampling rate, and check you have not enabled `sensors -j`, which forks a
process every sample:

```toml
[agent]
sample_interval = 5.0

[temperature]
use_sensors_json = false
```

## Panel

### Nothing on the screen

1. Is the backlight on? If the screen is black but faintly lit, the panel booted
   and the UI failed — check serial.
2. `pio device monitor -p COM6 -b 115200` and reset the board.
3. Colours inverted or garbled → the panel variant needs a different driver.
   `ILI9341_2_DRIVER` is set for the 2432S028R; some later revisions ship ST7789.

### Touch does not respond, or is offset

The raw XPT2046 corners are set for a typical 2432S028R. A panel that reads
noticeably off can be corrected without editing code:

```ini
build_flags =
    ${env:cyd.build_flags}
    -D FP_TOUCH_RAW_MIN_X=250
    -D FP_TOUCH_RAW_MAX_X=3650
    -D FP_TOUCH_RAW_MIN_Y=280
    -D FP_TOUCH_RAW_MAX_Y=3750
```

If touch does nothing at all, check the IRQ pin: the driver ignores anything that
does not assert it.

### Wi-Fi will not connect

Serial and the Wi-Fi screen both report the reason:

| Message | Meaning |
| ------- | ------- |
| `not found` | SSID not in range, or 5 GHz only — the ESP32 is 2.4 GHz only |
| `wrong password or rejected` | credentials, or MAC filtering |
| `timeout` | weak signal, or a captive portal on the network |

Retries back off to 60 s and rotate through saved networks. After roughly three
failures per saved network the setup portal comes up automatically, **without**
erasing anything.

### The panel forgot my network

It does not, unless asked. If it is showing the portal, the connection is failing —
the saved list is intact under Wi-Fi → Saved networks.

### Recovery: I cannot reach it at all

Hold a finger on the touchscreen while powering up. After three seconds (the RGB
LED turns amber) the setup portal starts and **all settings are preserved**.

### A device shows "offline" but the agent is fine

```bash
curl -s http://<agent-ip>:8770/api/v1/health
```

Then check the panel's log (System information page, or `nc wikistats.local 23`):

| Log line | Cause |
| -------- | ----- |
| `connection refused` | agent not running, or wrong port |
| `HTTP 401` | agent needs a token the panel does not have |
| `HTTP 403` | the panel's address is outside `trusted_networks` |
| `payload too large` | raise `max_payload_bytes`, or reduce the agent's mount list |
| `unsupported schema` | agent is much newer than the firmware |
| `device id mismatch` | a recycled DHCP lease now points at a different machine — delete and rediscover |

### Values are frozen but the device says online

That is the design working: the last known values stay on screen and the age counter
keeps rising. Watch the *UPDATED* field — once it passes `stale_s` the badge turns
amber, and past `offline_s` it turns red.

### MQTT connects but no data arrives

* Base topic mismatch. Agent `mqtt.base_topic` and panel *Base topic* must be
  identical.
* Broker ACLs blocking `fleetpanel/v1/devices/#`.
* Payload above `max_payload_bytes`.

Verify at the broker:

```bash
mosquitto_sub -h broker.lan -t 'fleetpanel/v1/#' -v | head -5
```

In `auto` mode the panel falls back to HTTP when MQTT stops delivering, so a silent
broker degrades rather than blanking the screen.

### Reboot loop

The reset reason is on the diagnostics screen and in the first log lines after boot:

```text
[     521][i][state] boot #63, reset reason: panic
```

`panic` with a `tcpip_api_call … Invalid mbox` backtrace means a network call ran
before Wi-Fi initialised lwIP. `task-watchdog` means something blocked the loop task.
Malformed telemetry cannot cause either — the parser rejects bad input and there are
tests for every malformed case.

### Out of flash when building

`Flash: 89.3%` is normal. If a change pushes it over, in order of impact: drop an
LVGL font from `lv_conf.h`, disable unused widgets, or keep `CORE_DEBUG_LEVEL=0`.

### Web interface will not load

* `http://wikistats.local/` fails → try the IP from the diagnostics screen; mDNS
  resolution from Windows needs Bonjour.
* Blank page → the LittleFS image was never uploaded:
  `pio run -e cyd -t uploadfs`.
* "Web configuration is disabled" → re-enable it from the touchscreen, or factory
  reset.
* Locked out → factory reset from Settings → Diagnostics → Factory.

## Collecting diagnostics

```bash
# agent
sudo journalctl -u fleetpanel-agent -n 200 --no-pager
curl -s http://127.0.0.1:8770/api/v1/info

# panel
nc wikistats.local 23
curl -s http://wikistats.local/api/status
```

Neither output contains a password or a token.
