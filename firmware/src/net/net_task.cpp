#include "net_task.h"

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>

#include "../app_state.h"
#include "../log.h"
#include "ota_service.h"
#include "screenshot.h"
#include "web_server.h"
#include "wifi_manager.h"

namespace net {

namespace {

constexpr const char* kTag = "net";
constexpr uint32_t kTickMs = 100;
constexpr uint32_t kTaskStackWords = 8192;   // HTTPClient + TLS need a deep stack
constexpr uint32_t kTaskPriority = 2;
constexpr BaseType_t kTaskCore = 0;
constexpr uint32_t kMaxBackoffMultiplier = 8;

HttpTelemetryTransport g_http;
MqttTelemetryTransport g_mqtt;
MdnsDiscovery g_mdns;

volatile bool g_scanRequested = false;
volatile bool g_reloadRequested = false;
bool g_servicesStarted = false;
uint32_t g_nextDiscoveryMs = 0;
size_t g_pollCursor = 0;
const char* g_activeTransport = "none";

// ------------------------------------------------------------- MQTT sinks

void onMqttSample(const std::string& deviceId, const fp::Telemetry& telemetry) {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    fp::DeviceState* device = state.devices().find(deviceId);
    if (device == nullptr) {
        return;  // not approved yet; the meta handler put it in the pending list
    }
    device->applySample(telemetry, millis(), fp::DeviceSource::Mqtt);
    ++state.status().mqttMessages;
    state.status().lastMqttSampleMs = millis();
    state.touch();
}

void onMqttMeta(const std::string& deviceId, const std::string& name, const std::string& baseUrl,
                const std::string& platform) {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    fp::DeviceConfig incoming;
    incoming.id = deviceId;
    incoming.name = name.empty() ? deviceId : name;
    incoming.baseUrl = baseUrl;
    incoming.platform = platform.empty() ? "linux" : platform;
    incoming.source = fp::DeviceSource::Mqtt;
    if (state.devices().upsertDiscovered(incoming, state.config().discovery.autoAdd)) {
        state.status().discoveredCount = state.devices().pending().size();
        state.touch();
        state.requestSave();
    }
}

void onMqttAvailability(const std::string& deviceId, bool online) {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    fp::DeviceState* device = state.devices().find(deviceId);
    if (device == nullptr) {
        return;
    }
    if (device->announcedOffline == !online) {
        return;
    }
    device->announcedOffline = !online;
    if (!online) {
        device->lastError = "agent reported offline";
        LOG_I(kTag, "%s went offline (Last Will)", deviceId.c_str());
    }
    state.touch();
}

// --------------------------------------------------------------- policy

// In `auto`, MQTT wins only while it is both connected and *delivering*. A broker
// that is up with no publisher would otherwise leave every tile blank forever.
bool shouldPollOverHttp(const fp::DeviceState& device, const fp::PanelConfig& config,
                        uint32_t nowMs) {
    switch (config.transport.mode) {
        case fp::TransportMode::Http:
            return true;
        case fp::TransportMode::Mqtt:
            return false;
        case fp::TransportMode::Auto:
        default:
            break;
    }
    if (!g_mqtt.connected()) {
        return true;
    }
    if (!device.everReceived || device.lastSampleSource != fp::DeviceSource::Mqtt) {
        return true;
    }
    return (nowMs - device.lastContactMs) > config.transport.mqttFreshMs;
}

uint32_t pollIntervalFor(const fp::DeviceState& device, const fp::PanelConfig& config) {
    // Back off on a device that keeps failing so one dead host does not consume the
    // polling budget of a healthy fleet.
    uint32_t multiplier = 1;
    for (uint32_t i = 0; i < device.consecutiveFailures && multiplier < kMaxBackoffMultiplier;
         ++i) {
        multiplier *= 2;
    }
    return config.transport.pollIntervalMs * multiplier;
}

// ------------------------------------------------------------ operations

void applyDiscovery(const std::vector<fp::DeviceConfig>& found) {
    if (found.empty()) {
        return;
    }
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    bool changed = false;
    const bool autoAdd = state.config().discovery.autoAdd;
    for (const fp::DeviceConfig& device : found) {
        changed |= state.devices().upsertDiscovered(device, autoAdd);
    }
    state.status().discoveredCount = state.devices().pending().size();
    if (changed) {
        state.touch();
        state.requestSave();
    }
}

// Polls at most one device per tick. Spreading the work keeps the task responsive to
// OTA and the log console even with a large fleet.
void pollOneDevice(uint32_t nowMs) {
    app::AppState& state = app::state();
    fp::DeviceConfig target;
    bool haveTarget = false;

    {
        app::AppState::Lock lock(state);
        std::vector<fp::DeviceState>& devices = state.devices().all();
        if (devices.empty()) {
            return;
        }
        const fp::PanelConfig& config = state.config();
        for (size_t i = 0; i < devices.size() && !haveTarget; ++i) {
            const size_t index = (g_pollCursor + i) % devices.size();
            fp::DeviceState& device = devices[index];
            if (!device.config.enabled || device.config.baseUrl.empty()) {
                continue;
            }
            if (!shouldPollOverHttp(device, config, nowMs)) {
                continue;
            }
            const uint32_t interval = pollIntervalFor(device, config);
            if (device.lastAttemptMs != 0 && (nowMs - device.lastAttemptMs) < interval) {
                continue;
            }
            target = device.config;
            haveTarget = true;
            g_pollCursor = (index + 1) % devices.size();
            // Claim it now so a slow fetch cannot be started twice.
            device.lastAttemptMs = nowMs;
        }
        g_http.setTimeoutMs(config.transport.httpTimeoutMs);
        g_http.setMaxPayloadBytes(config.transport.maxPayloadBytes);
    }

    if (!haveTarget) {
        return;
    }

    // The fetch itself happens with the lock released: it can take seconds.
    fp::Telemetry telemetry;
    std::string error;
    const FetchResult result = g_http.fetch(target, telemetry, error);
    const uint32_t completedMs = millis();

    app::AppState::Lock lock(state);
    fp::DeviceState* device = state.devices().find(target.id);
    if (device == nullptr) {
        return;  // deleted while the request was in flight
    }
    if (result == FetchResult::Ok) {
        device->applySample(telemetry, completedMs, fp::DeviceSource::Manual);
        ++state.status().httpPolls;
        g_activeTransport = "http";
    } else {
        device->noteFailure(error, completedMs);
        ++state.status().httpFailures;
        if (device->consecutiveFailures == 1 || device->consecutiveFailures % 20 == 0) {
            // First failure and then rarely: a device that is off for a week must
            // not fill the log ring.
            LOG_W(kTag, "%s: %s (%s)", target.id.c_str(), error.c_str(),
                  fetchResultName(result));
        }
    }
    state.touch();
}

void startOnlineServices() {
    if (g_servicesStarted) {
        return;
    }
    app::AppState& state = app::state();
    std::string hostname;
    bool telnetEnabled = false;
    uint16_t telnetPort = 23;
    std::string otaPassword;
    bool otaEnabled = false;
    bool mdnsEnabled = true;
    {
        app::AppState::Lock lock(state);
        hostname = state.config().wifi.hostname;
        telnetEnabled = state.config().logging.telnetEnabled;
        telnetPort = state.config().logging.telnetPort;
        otaEnabled = state.config().ota.arduinoOtaEnabled;
        // Fall back to the web admin hash so OTA is never left unauthenticated.
        otaPassword = state.config().ota.password.empty() ? state.config().web.passwordHash
                                                          : state.config().ota.password;
        mdnsEnabled = state.config().discovery.mdnsEnabled;
    }

    if (mdnsEnabled) {
        g_mdns.begin(hostname);
    }
    if (telnetEnabled) {
        fplog::startConsole(telnetPort);
        // The frame grabber sits on the next port up and follows the same switch:
        // both are read-only diagnostic channels, so they are enabled together.
        shot::begin(static_cast<uint16_t>(telnetPort + 1));
    }
    if (otaEnabled) {
        ota().begin(hostname, otaPassword);
    }

    {
        app::AppState::Lock lock(state);
        state.status().mdnsRunning = g_mdns.running();
        state.touch();
    }
    g_servicesStarted = true;
    g_nextDiscoveryMs = millis() + 2000;  // let the network settle before the first sweep
}

void stopOnlineServices() {
    if (!g_servicesStarted) {
        return;
    }
    g_mdns.end();
    ota().end();
    fplog::stopConsole();
    shot::end();
    g_servicesStarted = false;
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    state.status().mdnsRunning = false;
    state.touch();
}

void reloadTransports() {
    app::AppState& state = app::state();
    bool enabled = false;
    std::string host, username, password, baseTopic, clientId;
    uint16_t port = 1883;
    bool tls = false;
    uint32_t maxPayload = 16384;
    {
        app::AppState::Lock lock(state);
        const fp::MqttSettings& mqtt = state.config().mqtt;
        enabled = mqtt.enabled;
        host = mqtt.host;
        port = mqtt.port;
        username = mqtt.username;
        password = mqtt.password;
        baseTopic = mqtt.baseTopic;
        clientId = mqtt.clientId;
        tls = mqtt.tls;
        maxPayload = state.config().transport.maxPayloadBytes;
    }

    if (!enabled) {
        g_mqtt.end();
        return;
    }
    g_mqtt.setMaxPayloadBytes(maxPayload);
    g_mqtt.configure(host, port, username, password, baseTopic, clientId, tls);
    g_mqtt.onSample(onMqttSample);
    g_mqtt.onMeta(onMqttMeta);
    g_mqtt.onAvailability(onMqttAvailability);
    g_mqtt.begin();
}

// ------------------------------------------------------------- task body

void networkTask(void*) {
    LOG_I(kTag, "network task started on core %d", xPortGetCoreID());
    wifi().begin();
    // The web server must not start before this point. AsyncTCP opens an lwIP
    // socket in begin(), and lwIP is only initialised by the first WiFi.mode()
    // call - starting it from setup() panics with "tcpip_api_call ... Invalid mbox"
    // and reboot-loops the panel.
    startWebServer();
    g_http.begin();
    reloadTransports();

    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        const uint32_t now = millis();

        wifi().loop(now);
        fplog::pump();
        shot::poll();

        const bool online = WiFi.status() == WL_CONNECTED;
        if (online) {
            startOnlineServices();
        } else if (g_servicesStarted && !wifi().portalActive()) {
            stopOnlineServices();
        }

        if (g_reloadRequested) {
            g_reloadRequested = false;
            reloadTransports();
        }

        ota().loop();
        g_mqtt.loop(now);

        if (online) {
            uint32_t discoveryInterval = 60000;
            bool mdnsEnabled = true;
            {
                app::AppState& state = app::state();
                app::AppState::Lock lock(state);
                discoveryInterval = state.config().discovery.intervalSeconds * 1000u;
                mdnsEnabled = state.config().discovery.mdnsEnabled;
            }
            const bool due = mdnsEnabled && now >= g_nextDiscoveryMs;
            if (g_scanRequested || due) {
                g_scanRequested = false;
                g_nextDiscoveryMs = now + discoveryInterval;
                applyDiscovery(g_mdns.scan());
            }
            pollOneDevice(millis());
        }

        {
            app::AppState& state = app::state();
            app::AppState::Lock lock(state);
            const bool connected = g_mqtt.connected();
            if (state.status().mqttConnected != connected) {
                state.status().mqttConnected = connected;
                state.touch();
            }
        }

        app::state().flushPendingSave(millis());

        if (!online) {
            g_activeTransport = "offline";
        } else if (g_mqtt.connected() && (millis() - g_mqtt.lastSampleMs()) < 15000) {
            g_activeTransport = "mqtt";
        } else if (g_activeTransport == std::string("offline") ||
                   g_activeTransport == std::string("none")) {
            // Online but nothing has delivered a sample yet. Reporting "offline"
            // here was misleading: the panel is up, it simply has no data source.
            g_activeTransport = "idle";
        }

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(kTickMs));
    }
}

}  // namespace

void startNetworkTask() {
    xTaskCreatePinnedToCore(networkTask, "wikistats-net", kTaskStackWords, nullptr, kTaskPriority,
                            nullptr, kTaskCore);
}

void requestDiscoveryScan() { g_scanRequested = true; }
void requestTransportReload() { g_reloadRequested = true; }

HttpTelemetryTransport& httpTransport() { return g_http; }
MqttTelemetryTransport& mqttTransport() { return g_mqtt; }
MdnsDiscovery& mdns() { return g_mdns; }
const char* activeTransportName() { return g_activeTransport; }

}  // namespace net
