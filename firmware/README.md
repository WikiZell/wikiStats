# WikiStats panel firmware

ESP32 firmware for the **ESP32-2432S028R** ("Cheap Yellow Display"): 320 × 240
ILI9341, XPT2046 resistive touch, ESP32-WROOM-32, 4 MB flash, **no PSRAM**.

## Commands

```bash
pio run                    # build the default environment (cyd)
pio test -e native         # 120 host-side unit tests, no board needed
pio run -t upload          # flash over USB
pio run -t uploadfs        # flash the LittleFS image (web assets)
pio device monitor         # 115200 baud, with exception decoding
```

Build the web assets before `uploadfs`:

```bash
cd ../web && npm install && npm run build
```

Over the air, once a password is set:

```bash
pio run -e cyd-ota -t upload --upload-port wikistats-XXXX.local --upload-flags --auth=YOURPASSWORD
```

## Layout

```text
firmware/
├── platformio.ini              pinned dependencies, pin map, build flags
├── partitions_wikistats.csv    2 × 1856 KiB OTA slots + 256 KiB LittleFS
├── include/lv_conf.h           LVGL configuration
├── lib/fleet_core/             NO Arduino headers - host testable
│   ├── fp_telemetry.*          defensive fleetpanel.telemetry.v1 parser
│   ├── fp_units.*              KiB/MiB/GiB, rates, uptime, temperatures
│   ├── fp_thresholds.*         warning/critical + stale/offline
│   ├── fp_gesture.*            swipe / tap / long-press recognition
│   ├── fp_carousel.*           idle rotation timing
│   ├── fp_devices.*            registry, dedup, ordering, bounded history
│   ├── fp_config.*             versioned config, migration, redaction
│   └── fp_mqtt_topics.*        topic build + parse
├── src/
│   ├── main.cpp                startup order, recovery check, loop
│   ├── app_state.*             shared state, mutex, atomic persistence
│   ├── log.*                   serial + ring buffer + Wi-Fi console
│   ├── hal/                    display and touch abstraction
│   ├── net/                    transports, discovery, web server, OTA
│   └── ui/                     LVGL screens
├── data/www/                   gzipped web assets (generated)
└── test/test_core/             native unit tests
```

## Why this stack

`esp32_smartdisplay` was the suggested integration. This firmware uses **LVGL 8.3 +
TFT_eSPI + XPT2046_Touchscreen behind a local HAL** instead, for three reasons:

1. The CYD drives its touch controller on a **separate SPI bus**. TFT_eSPI's built-in
   touch support assumes a shared bus, so touch had to be driven separately anyway —
   at which point a thin HAL is less indirection, not more.
2. Pinning exact versions of three well-known libraries is more predictable than
   depending on a board-support package that pulls its own board JSON.
3. `src/hal/display_hal.h` is 60 lines. Supporting another variant means one new
   `.cpp` and one new `platformio.ini` environment, with nothing above it changing.

## Task model

| Task | Core | Owns | Blocks? |
| ---- | ---- | ---- | ------- |
| `loop()` / LVGL | 1 | touch, rendering, carousel | never |
| `wikistats-net` | 0 | Wi-Fi, mDNS, HTTP, MQTT, OTA, log console | yes, by design |
| AsyncTCP | 0 | web requests | no |

`HTTPClient` is a blocking API, and that is fine: it blocks the network task, which
has nothing else to do while it waits. A device that times out after four seconds
cannot drop a UI frame.

Shared state lives in one `AppState` behind a recursive mutex, with a revision
counter the UI watches. That is what makes a change in the browser appear on the
touchscreen immediately, and vice versa.

**Order matters at startup.** The web server is started from the network task, after
`WiFi.mode()`. Starting AsyncTCP before Wi-Fi has initialised lwIP panics with
`tcpip_api_call … Invalid mbox` and reboot-loops the panel — found on real hardware,
fixed, and commented at the call site.

## Memory

Measured on the board: **68 020 B static RAM (20.8 %)**, **1 698 289 B flash
(89.4 % of the 1856 KiB app slot)**, ~199 KB free heap after startup.

Everything that could grow without bound is capped: 25 KiB LVGL draw buffer,
16 KiB payload ceiling checked *before* parsing, ArduinoJson filter so only rendered
fields are allocated, 16 cores / 6 temperatures / 4 mounts per sample, 60-point
fixed-array history per device, 24 devices, 8 Wi-Fi networks, 6 KiB log ring,
16 KiB request body, 4 sessions. No `String` concatenation in any render or log path.

If a change pushes flash over the limit, in order of impact: drop an LVGL font from
`lv_conf.h`, disable unused widgets, keep `CORE_DEBUG_LEVEL=0`.

## Testing

```bash
pio test -e native
```

```text
120 test cases: 120 succeeded in 00:00:04
```

Covers telemetry parsing (including missing, null, unknown and wrong-typed fields),
unit formatting, threshold and freshness logic, carousel timing, device dedup and
ordering, configuration migration and secret redaction, swipe detection, and MQTT
topic parsing.

On Windows the native environment needs a host compiler on `PATH` (MinGW-w64 or
similar); on Linux and macOS the system toolchain is enough.

## Configuration on flash

`/config.json` on LittleFS, written atomically: `config.tmp` → read back and
re-parsed → `config.json` rotated to `config.bak` → `config.tmp` renamed into place.
Power loss at any point leaves at least one complete, parseable document.

The partition is labelled `littlefs`, not the Arduino default of `spiffs`, so
`LittleFS.begin()` is called with the label explicitly — omitting it fails with
`partition "spiffs" could not be found`.

## Serial and remote diagnostics

Every log line goes to USB serial, a 6 KiB ring buffer, and TCP port 23 once Wi-Fi
is up:

```bash
nc wikistats-XXXX.local 23
```

Output only — anything typed at it is read and discarded, so it can never become a
command channel. `GET /api/logs` returns the same buffer.

## Screenshots

```bash
python tools/screenshot.py wikistats-XXXX.local dashboard.png
```

A full 320x240 RGB565 frame is 150 KiB and this board has around 100 KiB of heap
free, so the frame is never buffered. A client connects to TCP 24, the UI task
invalidates the screen, and each ~40-line band goes to the socket straight out of
the LVGL flush callback. The cost is that the UI stalls for the length of the
capture, which is fine for something that runs on demand and invaluable for
reviewing layout on a panel you cannot reach.

The listener follows the same configuration switch as the log console, on the next
port up.

## Recovery

Hold the touchscreen during boot. After three seconds (RGB LED turns amber) the
setup portal starts with **all settings preserved**.
