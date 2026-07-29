// WikiStats / FleetPanel panel firmware entry point.
//
// Task layout:
//   core 1, loop()  - LVGL: touch, rendering, animations. Never blocks.
//   core 0, net     - Wi-Fi, mDNS, HTTP polling, MQTT, OTA, log console.
//   core 0, async   - ESPAsyncWebServer's own task (created by the library).
//
// Startup order is deliberate: storage and configuration first (so the display uses
// the saved rotation and brightness), then the display, then the recovery check
// while the user's finger is still likely on the glass, then the UI, then the
// network. Nothing that can block runs before the first frame is on screen.

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "app_state.h"
#include "hal/display_hal.h"
#include "log.h"
#include "net/net_task.h"
#include "net/web_server.h"
#include "net/wifi_manager.h"
#include "ui/ui.h"

namespace {

constexpr const char* kTag = "main";

// Hold the touchscreen for this long during boot to force the setup portal.
constexpr uint32_t kRecoveryHoldMs = 3000;
constexpr uint32_t kRecoveryPollMs = 50;

void banner() {
    LOG_I(kTag, "%s %s", FP_PRODUCT_NAME, FP_FIRMWARE_VERSION);
    LOG_I(kTag, "telemetry schema %s", FP_TELEMETRY_SCHEMA);
    LOG_I(kTag, "chip %s rev %d, %d MHz, flash %u MB", ESP.getChipModel(), ESP.getChipRevision(),
          ESP.getCpuFreqMHz(), (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    LOG_I(kTag, "free heap %u B, sketch %u B, free sketch space %u B", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getSketchSize(), (unsigned)ESP.getFreeSketchSpace());
}

// Physical recovery. The check runs before the UI exists, so it draws its own
// countdown straight onto the panel.
bool touchHeldDuringBoot() {
    hal::TouchPoint point;
    if (!hal::display().readTouch(point)) {
        return false;
    }
    LOG_W(kTag, "touch detected at boot; hold for %lu ms to open the setup portal",
          (unsigned long)kRecoveryHoldMs);
    const uint32_t deadline = millis() + kRecoveryHoldMs;
    while (millis() < deadline) {
        if (!hal::display().readTouch(point)) {
            LOG_I(kTag, "touch released; continuing normal boot");
            return false;
        }
        // Amber while the timer runs, so the user gets feedback with no UI yet.
        hal::display().setStatusLed(255, 180, 0);
        delay(kRecoveryPollMs);
    }
    hal::display().setStatusLed(0, 0, 0);
    LOG_W(kTag, "recovery: forcing the Wi-Fi setup portal (settings are preserved)");
    return true;
}

}  // namespace

void setup() {
    fplog::begin(115200);
    banner();

    // Configuration first: the display rotation and brightness come from it.
    app::state().begin();
    {
        app::AppState& state = app::state();
        app::AppState::Lock lock(state);
        fplog::setLevel(fplog::levelFromName(state.config().logging.level.c_str()));
    }

    uint8_t rotation = 1;
    {
        app::AppState& state = app::state();
        app::AppState::Lock lock(state);
        rotation = state.config().display.rotation;
    }
    if (!hal::display().begin(rotation)) {
        LOG_E(kTag, "display init failed");
    }
    LOG_I(kTag, "panel: %s (%s + %s)", hal::display().info().boardName,
          hal::display().info().panelDriver, hal::display().info().touchDriver);

    if (touchHeldDuringBoot()) {
        net::wifi().forcePortalOnBoot();
    }

    if (!ui::begin()) {
        // Without a UI the panel is still useful over the web interface, so this is
        // reported rather than fatal.
        LOG_E(kTag, "UI init failed; continuing headless");
    }

    // The web server is started by the network task, after Wi-Fi has brought lwIP
    // up. Starting it here would panic inside AsyncTCP.
    net::startNetworkTask();

    LOG_I(kTag, "startup complete, free heap %u B", (unsigned)ESP.getFreeHeap());
}

void loop() {
    ui::tick();

    if (net::factoryResetPending()) {
        net::clearPendingActions();
        LOG_W(kTag, "factory reset requested over the web API");
        app::state().factoryReset();
        delay(200);
        ESP.restart();
    }
    if (net::restartPending()) {
        net::clearPendingActions();
        LOG_W(kTag, "restart requested over the web API");
        // Give the HTTP response time to leave the socket before the reboot.
        delay(500);
        ESP.restart();
    }

    // 5 ms keeps LVGL's 20 ms refresh and 20 ms input period comfortably fed while
    // leaving the core available to the IDLE task, which is what feeds the watchdog.
    delay(5);
}
