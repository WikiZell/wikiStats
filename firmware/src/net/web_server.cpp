#include "web_server.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include <cstring>

#include "../app_state.h"
#include "../log.h"
#include "net_task.h"
#include "wifi_manager.h"

namespace net {

namespace {

constexpr const char* kTag = "web";
constexpr uint16_t kPort = 80;
constexpr size_t kMaxBodyBytes = 16 * 1024;
constexpr uint32_t kPbkdf2Iterations = 10000;
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;
constexpr size_t kMaxSessions = 4;
constexpr size_t kMaxRateEntries = 8;
constexpr uint32_t kBaseLockoutMs = 5000;
constexpr uint32_t kMaxLockoutMs = 300000;
constexpr uint8_t kFailuresBeforeLockout = 5;
constexpr size_t kLogSnapshotBytes = 6144;

AsyncWebServer g_server(kPort);
bool g_running = false;
volatile bool g_restartPending = false;
volatile bool g_factoryResetPending = false;

struct Session {
    char token[65] = {0};
    char csrf[33] = {0};
    uint32_t expiresAtMs = 0;
    bool used = false;
};
Session g_sessions[kMaxSessions];

struct RateEntry {
    uint32_t ip = 0;
    uint8_t failures = 0;
    uint32_t lockedUntilMs = 0;
};
RateEntry g_rate[kMaxRateEntries];

// ------------------------------------------------------------- utilities

void randomHex(char* out, size_t hexChars) {
    static const char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < hexChars; ++i) {
        // esp_random() is the hardware RNG; it is seeded by the RF subsystem and is
        // the right source for session tokens.
        out[i] = kDigits[esp_random() & 0x0F];
    }
    out[hexChars] = '\0';
}

std::string toHex(const uint8_t* data, size_t length) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

bool fromHex(const std::string& hex, uint8_t* out, size_t expected) {
    if (hex.size() != expected * 2) {
        return false;
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < expected; ++i) {
        const int hi = nibble(hex[i * 2]);
        const int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// Constant time: a timing difference here would leak the token one byte at a time.
bool secureEquals(const char* a, const char* b, size_t length) {
    uint8_t diff = 0;
    for (size_t i = 0; i < length; ++i) {
        diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
    }
    return diff == 0;
}

bool pbkdf2(const std::string& password, const uint8_t* salt, size_t saltLength, uint8_t* out) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr || mbedtls_md_setup(&ctx, info, 1) != 0) {
        mbedtls_md_free(&ctx);
        return false;
    }
    const int rc = mbedtls_pkcs5_pbkdf2_hmac(
        &ctx, reinterpret_cast<const unsigned char*>(password.c_str()), password.size(), salt,
        saltLength, kPbkdf2Iterations, kHashBytes, out);
    mbedtls_md_free(&ctx);
    return rc == 0;
}

uint32_t clientIp(AsyncWebServerRequest* request) {
    return static_cast<uint32_t>(request->client()->remoteIP());
}

// ---------------------------------------------------------- rate limiting

RateEntry& rateEntryFor(uint32_t ip) {
    for (RateEntry& entry : g_rate) {
        if (entry.ip == ip) {
            return entry;
        }
    }
    RateEntry* oldest = &g_rate[0];
    for (RateEntry& entry : g_rate) {
        if (entry.ip == 0) {
            entry = RateEntry{};
            entry.ip = ip;
            return entry;
        }
        if (entry.lockedUntilMs < oldest->lockedUntilMs) {
            oldest = &entry;
        }
    }
    *oldest = RateEntry{};
    oldest->ip = ip;
    return *oldest;
}

bool rateLimited(uint32_t ip, uint32_t& retryAfterMs) {
    const RateEntry& entry = rateEntryFor(ip);
    const uint32_t now = millis();
    if (entry.lockedUntilMs > now) {
        retryAfterMs = entry.lockedUntilMs - now;
        return true;
    }
    return false;
}

void noteLoginFailure(uint32_t ip) {
    RateEntry& entry = rateEntryFor(ip);
    if (entry.failures < 255) {
        ++entry.failures;
    }
    if (entry.failures >= kFailuresBeforeLockout) {
        // Doubling lockout: five quick guesses cost 5 s, the next five cost 10 s,
        // and an unattended brute force stalls at five minutes per attempt.
        const uint32_t steps = entry.failures - kFailuresBeforeLockout;
        uint32_t lockout = kBaseLockoutMs;
        for (uint32_t i = 0; i < steps && lockout < kMaxLockoutMs; ++i) {
            lockout *= 2;
        }
        entry.lockedUntilMs = millis() + (lockout > kMaxLockoutMs ? kMaxLockoutMs : lockout);
    }
}

void noteLoginSuccess(uint32_t ip) {
    RateEntry& entry = rateEntryFor(ip);
    entry.failures = 0;
    entry.lockedUntilMs = 0;
}

// -------------------------------------------------------------- sessions

Session* createSession(uint32_t timeoutSeconds) {
    Session* slot = nullptr;
    const uint32_t now = millis();
    for (Session& session : g_sessions) {
        if (!session.used || session.expiresAtMs < now) {
            slot = &session;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &g_sessions[0];  // evict the first; four concurrent admins is plenty
    }
    randomHex(slot->token, 64);
    randomHex(slot->csrf, 32);
    slot->expiresAtMs = now + timeoutSeconds * 1000u;
    slot->used = true;
    return slot;
}

Session* findSession(AsyncWebServerRequest* request) {
    if (!request->hasHeader("Cookie")) {
        return nullptr;
    }
    const String cookies = request->header("Cookie");
    const int at = cookies.indexOf("wsid=");
    if (at < 0) {
        return nullptr;
    }
    String token = cookies.substring(at + 5);
    const int end = token.indexOf(';');
    if (end >= 0) {
        token = token.substring(0, end);
    }
    token.trim();
    if (token.length() != 64) {
        return nullptr;
    }
    const uint32_t now = millis();
    for (Session& session : g_sessions) {
        if (!session.used || session.expiresAtMs < now) {
            continue;
        }
        if (secureEquals(session.token, token.c_str(), 64)) {
            return &session;
        }
    }
    return nullptr;
}

void dropSession(AsyncWebServerRequest* request) {
    if (Session* session = findSession(request)) {
        *session = Session{};
    }
}

// ------------------------------------------------------------ auth gates

bool passwordConfigured() {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    return state.config().web.passwordSet;
}

bool webEnabled() {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    return state.config().web.enabled;
}

// During first-boot provisioning there is no password yet and no way to set one
// remotely without first joining a network. The portal is an open AP in physical
// range, which is the documented trust boundary for these three endpoints only.
bool provisioningWindow() { return wifi().portalActive() && !passwordConfigured(); }

void sendJson(AsyncWebServerRequest* request, int code, const JsonDocument& doc) {
    String body;
    serializeJson(doc, body);
    AsyncWebServerResponse* response = request->beginResponse(code, "application/json", body);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void sendError(AsyncWebServerRequest* request, int code, const char* message) {
    JsonDocument doc;
    doc["error"] = message;
    sendJson(request, code, doc);
}

// Returns true when the request may proceed; sends the error response otherwise.
bool requireAuth(AsyncWebServerRequest* request, bool mutating) {
    if (!webEnabled()) {
        sendError(request, 403, "web configuration is disabled on this panel");
        return false;
    }
    if (!passwordConfigured()) {
        if (!mutating || provisioningWindow()) {
            return true;
        }
        sendError(request, 403, "set an administrator password before changing settings");
        return false;
    }
    Session* session = findSession(request);
    if (session == nullptr) {
        sendError(request, 401, "authentication required");
        return false;
    }
    if (mutating) {
        // CSRF: a cookie alone is not enough, because a browser attaches it to
        // cross-site requests too. The token is only readable by same-origin script.
        if (!request->hasHeader("X-CSRF-Token")) {
            sendError(request, 403, "missing CSRF token");
            return false;
        }
        const String presented = request->header("X-CSRF-Token");
        if (presented.length() != 32 || !secureEquals(session->csrf, presented.c_str(), 32)) {
            sendError(request, 403, "invalid CSRF token");
            return false;
        }
    }
    // Sliding expiry: an admin actively using the UI is not logged out mid-edit.
    app::AppState& state = app::state();
    uint32_t timeout = 3600;
    {
        app::AppState::Lock lock(state);
        timeout = state.config().web.sessionTimeoutSeconds;
    }
    session->expiresAtMs = millis() + timeout * 1000u;
    return true;
}

// --------------------------------------------------------- body handling

const char* bodyOf(AsyncWebServerRequest* request, size_t& lengthOut) {
    if (request->_tempObject == nullptr) {
        lengthOut = 0;
        return nullptr;
    }
    const char* text = static_cast<const char*>(request->_tempObject);
    lengthOut = strlen(text);
    return text;
}

void collectBody(AsyncWebServerRequest* request, uint8_t* data, size_t length, size_t index,
                 size_t total) {
    if (total == 0 || total > kMaxBodyBytes) {
        return;  // the handler reports the error; allocating here would be the bug
    }
    if (index == 0) {
        if (request->_tempObject != nullptr) {
            free(request->_tempObject);
        }
        request->_tempObject = malloc(total + 1);
        if (request->_tempObject == nullptr) {
            return;
        }
        memset(request->_tempObject, 0, total + 1);
    }
    if (request->_tempObject == nullptr) {
        return;
    }
    memcpy(static_cast<uint8_t*>(request->_tempObject) + index, data, length);
}

// ------------------------------------------------------------- documents

void buildStatus(JsonDocument& doc) {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    const app::RuntimeStatus& status = state.status();

    doc["product"] = FP_PRODUCT_NAME;
    doc["firmware"] = FP_FIRMWARE_VERSION;
    doc["telemetry_schema"] = FP_TELEMETRY_SCHEMA;
    doc["uptime_ms"] = millis();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["min_free_heap"] = ESP.getMinFreeHeap();
    doc["heap_size"] = ESP.getHeapSize();
    doc["sketch_size"] = ESP.getSketchSize();
    doc["free_sketch_space"] = ESP.getFreeSketchSpace();
    doc["chip_model"] = ESP.getChipModel();
    doc["cpu_mhz"] = ESP.getCpuFreqMHz();
    doc["reset_reason"] = status.resetReason;
    doc["boot_count"] = status.bootCount;
    doc["config_status"] = status.configStatus;
    doc["password_set"] = state.config().web.passwordSet;
    doc["fs_total"] = LittleFS.totalBytes();
    doc["fs_used"] = LittleFS.usedBytes();

    JsonObject wifiObj = doc["wifi"].to<JsonObject>();
    wifiObj["phase"] = app::wifiPhaseName(status.wifi);
    wifiObj["ssid"] = status.ssid;
    wifiObj["ip"] = status.ip;
    wifiObj["rssi"] = status.rssi;
    wifiObj["ap_ssid"] = status.apSsid;
    wifiObj["ap_ip"] = status.apIp;
    wifiObj["hostname"] = state.config().wifi.hostname;
    wifiObj["saved_networks"] = state.config().wifi.networks.size();
    wifiObj["message"] = wifi().lastMessage();

    JsonObject transport = doc["transport"].to<JsonObject>();
    transport["mode"] = fp::transportModeName(state.config().transport.mode);
    transport["active"] = activeTransportName();
    transport["mqtt_connected"] = status.mqttConnected;
    transport["mqtt_messages"] = status.mqttMessages;
    transport["http_polls"] = status.httpPolls;
    transport["http_failures"] = status.httpFailures;
    transport["mdns_running"] = status.mdnsRunning;
    transport["pending_discoveries"] = state.devices().pending().size();

    doc["device_count"] = state.devices().size();
    doc["log_console_clients"] = fplog::consoleClients();
}

void appendDevice(JsonObject item, const fp::DeviceState& device, uint32_t nowMs,
                  const fp::Thresholds& thresholds) {
    item["id"] = device.config.id;
    item["name"] = device.config.name;
    item["alias"] = device.config.alias;
    item["label"] = device.config.label();
    item["base_url"] = device.config.baseUrl;
    item["path"] = device.config.path;
    item["platform"] = device.config.platform;
    item["source"] = fp::deviceSourceName(device.config.source);
    item["auth"] = fp::deviceAuthName(device.config.auth);
    // Never the token itself; only whether one is configured.
    item["token_set"] = !device.config.token.empty();
    item["enabled"] = device.config.enabled;
    item["hidden"] = device.config.hidden;
    item["carousel"] = device.config.carousel;
    item["order"] = device.config.order;
    item["ever_received"] = device.everReceived;
    item["age_seconds"] = device.ageSeconds(nowMs);
    item["failures"] = device.consecutiveFailures;
    item["last_error"] = device.lastError;
    item["last_transport"] = fp::deviceSourceName(device.lastSampleSource);

    const fp::Freshness freshness =
        device.announcedOffline
            ? fp::Freshness::Offline
            : fp::freshnessFor(device.ageSeconds(nowMs), device.everReceived, thresholds);
    item["state"] = fp::freshnessName(freshness);

    if (!device.everReceived) {
        return;
    }
    JsonObject metrics = item["metrics"].to<JsonObject>();
    const fp::Telemetry& t = device.latest;
    if (t.cpuPercent.has) metrics["cpu_percent"] = t.cpuPercent.value;
    if (t.cpuTemperature.has) metrics["cpu_temperature_c"] = t.cpuTemperature.value;
    if (t.memPercent.has) metrics["memory_percent"] = t.memPercent.value;
    if (t.memTotal.has) metrics["memory_total_bytes"] = t.memTotal.value;
    if (t.memUsed.has) metrics["memory_used_bytes"] = t.memUsed.value;
    if (t.diskPercent.has) metrics["storage_percent"] = t.diskPercent.value;
    if (t.diskTotal.has) metrics["storage_total_bytes"] = t.diskTotal.value;
    if (t.diskFree.has) metrics["storage_free_bytes"] = t.diskFree.value;
    if (t.rxRate.has) metrics["rx_bytes_per_second"] = t.rxRate.value;
    if (t.txRate.has) metrics["tx_bytes_per_second"] = t.txRate.value;
    if (t.uptimeSeconds.has) metrics["uptime_seconds"] = t.uptimeSeconds.value;
    metrics["os_name"] = t.osName;
    metrics["hardware_model"] = t.hardwareModel;
    metrics["agent_version"] = t.agentVersion;
}

// -------------------------------------------------------------- handlers

void handleStatus(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }
    JsonDocument doc;
    buildStatus(doc);
    sendJson(request, 200, doc);
}

void handleSession(AsyncWebServerRequest* request) {
    JsonDocument doc;
    const Session* session = findSession(request);
    doc["authenticated"] = session != nullptr;
    doc["password_set"] = passwordConfigured();
    doc["provisioning"] = provisioningWindow();
    doc["web_enabled"] = webEnabled();
    if (session != nullptr) {
        doc["csrf"] = session->csrf;
    }
    sendJson(request, 200, doc);
}

void handleLogin(AsyncWebServerRequest* request) {
    size_t length = 0;
    const char* body = bodyOf(request, length);
    if (body == nullptr) {
        sendError(request, 400, "missing body");
        return;
    }
    uint32_t retryAfter = 0;
    if (rateLimited(clientIp(request), retryAfter)) {
        JsonDocument doc;
        doc["error"] = "too many attempts";
        doc["retry_after_ms"] = retryAfter;
        sendJson(request, 429, doc);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body, length)) {
        sendError(request, 400, "invalid JSON");
        return;
    }
    const std::string password = doc["password"] | "";
    if (!verifyAdminPassword(password)) {
        noteLoginFailure(clientIp(request));
        LOG_W(kTag, "failed login from %s", request->client()->remoteIP().toString().c_str());
        sendError(request, 401, "invalid password");
        return;
    }
    noteLoginSuccess(clientIp(request));

    uint32_t timeout = 3600;
    {
        app::AppState& state = app::state();
        app::AppState::Lock lock(state);
        timeout = state.config().web.sessionTimeoutSeconds;
    }
    Session* session = createSession(timeout);

    JsonDocument out;
    out["authenticated"] = true;
    out["csrf"] = session->csrf;
    String bodyText;
    serializeJson(out, bodyText);
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", bodyText);
    char cookie[128];
    snprintf(cookie, sizeof(cookie), "wsid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%lu",
             session->token, (unsigned long)timeout);
    response->addHeader("Set-Cookie", cookie);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    LOG_I(kTag, "admin logged in from %s", request->client()->remoteIP().toString().c_str());
}

void handleLogout(AsyncWebServerRequest* request) {
    dropSession(request);
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", "{\"ok\":true}");
    response->addHeader("Set-Cookie", "wsid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    request->send(response);
}

void handleSetPassword(AsyncWebServerRequest* request) {
    size_t length = 0;
    const char* body = bodyOf(request, length);
    if (body == nullptr) {
        sendError(request, 400, "missing body");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body, length)) {
        sendError(request, 400, "invalid JSON");
        return;
    }
    const std::string next = doc["password"] | "";

    // Changing an existing password requires proving you know the old one, even
    // with a valid session: a borrowed browser must not become a permanent takeover.
    if (passwordConfigured()) {
        if (!requireAuth(request, true)) {
            return;
        }
        const std::string current = doc["current_password"] | "";
        if (!verifyAdminPassword(current)) {
            sendError(request, 403, "current password is incorrect");
            return;
        }
    } else if (!provisioningWindow() && !requireAuth(request, false)) {
        return;
    }

    std::string error;
    if (!setAdminPassword(next, error)) {
        sendError(request, 400, error.c_str());
        return;
    }
    // Every existing session is invalidated: a password change should log out
    // anyone who was already in.
    for (Session& session : g_sessions) {
        session = Session{};
    }
    sendJson(request, 200, [] {
        JsonDocument doc;
        doc["ok"] = true;
        return doc;
    }());
    LOG_W(kTag, "administrator password changed");
}

void handleGetConfig(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }
    // Redacted: this document travels over plain HTTP on a LAN.
    const std::string json = app::state().backup(/*includeSecrets=*/false);
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json.c_str());
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void handlePutConfig(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    size_t length = 0;
    const char* body = bodyOf(request, length);
    if (body == nullptr) {
        sendError(request, 400, "missing body");
        return;
    }
    app::AppState& state = app::state();
    fp::ConfigResult result;
    {
        app::AppState::Lock lock(state);
        result = fp::mergeConfigPatch(body, length, state.config());
        if (result.status == fp::ConfigStatus::Ok) {
            state.syncRegistryFromConfig();
            state.touch();
            state.requestSave();
        }
    }
    if (result.status != fp::ConfigStatus::Ok) {
        sendError(request, 400, result.message.empty() ? "invalid configuration"
                                                       : result.message.c_str());
        return;
    }
    requestTransportReload();
    JsonDocument doc;
    doc["ok"] = true;
    doc["revision"] = state.revision();
    sendJson(request, 200, doc);
}

void handleGetDevices(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }
    JsonDocument doc;
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    const uint32_t now = millis();
    const fp::Thresholds& thresholds = state.config().thresholds;

    JsonArray devices = doc["devices"].to<JsonArray>();
    for (const size_t index : state.devices().visibleOrder()) {
        appendDevice(devices.add<JsonObject>(), state.devices().all()[index], now, thresholds);
    }
    // Hidden and disabled devices still appear in the management list, flagged.
    for (const fp::DeviceState& device : state.devices().all()) {
        if (device.config.enabled && !device.config.hidden) {
            continue;
        }
        appendDevice(devices.add<JsonObject>(), device, now, thresholds);
    }

    JsonArray pending = doc["discovered"].to<JsonArray>();
    for (const fp::DeviceConfig& device : state.devices().pending()) {
        JsonObject item = pending.add<JsonObject>();
        item["id"] = device.id;
        item["name"] = device.name;
        item["base_url"] = device.baseUrl;
        item["platform"] = device.platform;
        item["source"] = fp::deviceSourceName(device.source);
        item["auth"] = fp::deviceAuthName(device.auth);
    }
    sendJson(request, 200, doc);
}

fp::DeviceConfig deviceFromJson(const JsonDocument& doc, const fp::DeviceConfig& base) {
    fp::DeviceConfig device = base;
    if (doc["id"].is<const char*>()) device.id = doc["id"].as<const char*>();
    if (doc["name"].is<const char*>()) device.name = doc["name"].as<const char*>();
    if (doc["alias"].is<const char*>()) device.alias = doc["alias"].as<const char*>();
    if (doc["base_url"].is<const char*>()) device.baseUrl = doc["base_url"].as<const char*>();
    if (doc["path"].is<const char*>()) device.path = doc["path"].as<const char*>();
    if (doc["platform"].is<const char*>()) device.platform = doc["platform"].as<const char*>();
    if (doc["auth"].is<const char*>()) {
        device.auth = fp::deviceAuthFromName(doc["auth"].as<const char*>());
    }
    if (doc["token"].is<const char*>()) {
        const std::string token = doc["token"].as<const char*>();
        if (token != fp::kRedacted) {
            device.token = token;
        }
    }
    if (doc["enabled"].is<bool>()) device.enabled = doc["enabled"].as<bool>();
    if (doc["hidden"].is<bool>()) device.hidden = doc["hidden"].as<bool>();
    if (doc["carousel"].is<bool>()) device.carousel = doc["carousel"].as<bool>();
    if (doc["order"].is<uint8_t>()) device.order = doc["order"].as<uint8_t>();
    return device;
}

void handlePostDevice(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    size_t length = 0;
    const char* body = bodyOf(request, length);
    if (body == nullptr) {
        sendError(request, 400, "missing body");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body, length)) {
        sendError(request, 400, "invalid JSON");
        return;
    }

    fp::DeviceConfig device = deviceFromJson(doc, fp::DeviceConfig{});
    device.source = fp::DeviceSource::Manual;
    if (device.baseUrl.empty()) {
        sendError(request, 400, "base_url is required");
        return;
    }
    if (device.id.empty()) {
        // Manual entries by hostname have no stable ID until the first successful
        // poll. A provisional ID derived from the URL keeps the entry addressable
        // and is replaced when the agent reports its own.
        device.id = "manual-";
        for (char c : device.baseUrl) {
            if (isalnum(static_cast<unsigned char>(c))) {
                device.id.push_back(static_cast<char>(tolower(c)));
            }
            if (device.id.size() >= 24) {
                break;
            }
        }
    }
    if (device.name.empty()) {
        device.name = device.baseUrl;
    }

    app::AppState& state = app::state();
    bool ok = false;
    {
        app::AppState::Lock lock(state);
        ok = state.devices().upsertManual(device);
        if (ok) {
            state.touch();
            state.requestSave();
        }
    }
    if (!ok) {
        sendError(request, 409, "device list is full");
        return;
    }
    JsonDocument out;
    out["ok"] = true;
    out["id"] = device.id;
    sendJson(request, 201, out);
}

void handlePutDevice(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    const String id = request->pathArg(0);
    size_t length = 0;
    const char* body = bodyOf(request, length);
    if (body == nullptr) {
        sendError(request, 400, "missing body");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body, length)) {
        sendError(request, 400, "invalid JSON");
        return;
    }

    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    fp::DeviceState* existing = state.devices().find(id.c_str());
    if (existing == nullptr) {
        sendError(request, 404, "no such device");
        return;
    }
    fp::DeviceConfig updated = deviceFromJson(doc, existing->config);
    updated.id = existing->config.id;  // the ID is the key; it is never editable
    existing->config = updated;
    state.touch();
    state.requestSave();

    JsonDocument out;
    out["ok"] = true;
    sendJson(request, 200, out);
}

void handleDeleteDevice(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    const String id = request->pathArg(0);
    app::AppState& state = app::state();
    bool removed = false;
    {
        app::AppState::Lock lock(state);
        removed = state.devices().remove(id.c_str());
        if (!removed) {
            removed = state.devices().rejectPending(id.c_str());
        }
        if (removed) {
            state.touch();
            state.requestSave();
        }
    }
    if (!removed) {
        sendError(request, 404, "no such device");
        return;
    }
    JsonDocument out;
    out["ok"] = true;
    sendJson(request, 200, out);
}

void handleApproveDevice(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    const String id = request->pathArg(0);
    app::AppState& state = app::state();
    bool ok = false;
    {
        app::AppState::Lock lock(state);
        ok = state.devices().approve(id.c_str());
        if (ok) {
            state.status().discoveredCount = state.devices().pending().size();
            state.touch();
            state.requestSave();
        }
    }
    if (!ok) {
        sendError(request, 404, "not in the discovered list, or the device list is full");
        return;
    }
    JsonDocument out;
    out["ok"] = true;
    sendJson(request, 200, out);
}

void handleReorder(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    size_t length = 0;
    const char* body = bodyOf(request, length);
    if (body == nullptr) {
        sendError(request, 400, "missing body");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body, length)) {
        sendError(request, 400, "invalid JSON");
        return;
    }
    std::vector<std::string> ids;
    for (JsonVariantConst item : doc["order"].as<JsonArrayConst>()) {
        if (item.is<const char*>()) {
            ids.emplace_back(item.as<const char*>());
        }
    }
    app::AppState& state = app::state();
    {
        app::AppState::Lock lock(state);
        state.devices().reorder(ids);
        state.touch();
        state.requestSave();
    }
    JsonDocument out;
    out["ok"] = true;
    sendJson(request, 200, out);
}

void handleDiscoveryScan(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    requestDiscoveryScan();
    JsonDocument out;
    out["ok"] = true;
    out["note"] = "scan queued; results appear within a few seconds";
    sendJson(request, 202, out);
}

void handleWifiScan(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false) && !provisioningWindow()) {
        return;
    }
    JsonDocument doc;
    doc["scanning"] = wifi().scanInProgress();
    JsonArray networks = doc["networks"].to<JsonArray>();
    for (const ScanEntry& entry : wifi().scanResults()) {
        JsonObject item = networks.add<JsonObject>();
        item["ssid"] = entry.ssid;
        item["rssi"] = entry.rssi;
        item["channel"] = entry.channel;
        item["secure"] = entry.secure;
        item["known"] = entry.known;
    }
    if (!wifi().scanInProgress()) {
        wifi().requestScan();  // refresh for the next poll
    }
    sendJson(request, 200, doc);
}

void handleWifiConnect(AsyncWebServerRequest* request) {
    if (!provisioningWindow() && !requireAuth(request, true)) {
        return;
    }
    size_t length = 0;
    const char* body = bodyOf(request, length);
    if (body == nullptr) {
        sendError(request, 400, "missing body");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body, length)) {
        sendError(request, 400, "invalid JSON");
        return;
    }
    const std::string ssid = doc["ssid"] | "";
    const std::string password = doc["password"] | "";
    const bool hidden = doc["hidden"] | false;
    if (ssid.empty()) {
        sendError(request, 400, "ssid is required");
        return;
    }
    const bool started = wifi().connectTo(ssid, password, /*remember=*/true, hidden);
    JsonDocument out;
    out["ok"] = started;
    out["message"] = wifi().lastMessage();
    sendJson(request, started ? 202 : 400, out);
}

void handleWifiForget(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    size_t length = 0;
    const char* body = bodyOf(request, length);
    if (body == nullptr) {
        sendError(request, 400, "missing body");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body, length)) {
        sendError(request, 400, "invalid JSON");
        return;
    }
    const std::string ssid = doc["ssid"] | "";
    if (ssid.empty()) {
        sendError(request, 400, "ssid is required");
        return;
    }
    wifi().forget(ssid);
    JsonDocument out;
    out["ok"] = true;
    sendJson(request, 200, out);
}

void handleWifiForgetAll(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    wifi().forgetAll();
    JsonDocument out;
    out["ok"] = true;
    sendJson(request, 200, out);
}

void handleLogs(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }
    // A plain-text body keeps this readable with curl and avoids escaping a few
    // kilobytes of log into JSON.
    char* buffer = static_cast<char*>(malloc(kLogSnapshotBytes));
    if (buffer == nullptr) {
        sendError(request, 503, "out of memory");
        return;
    }
    fplog::snapshot(buffer, kLogSnapshotBytes);
    AsyncWebServerResponse* response = request->beginResponse(200, "text/plain", buffer);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    free(buffer);
}

void handleBackup(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }
    // Secrets only on explicit request, and only over an authenticated session.
    const bool includeSecrets = request->hasParam("secrets") &&
                                request->getParam("secrets")->value() == "1";
    if (includeSecrets && !passwordConfigured()) {
        sendError(request, 403, "set an administrator password before exporting secrets");
        return;
    }
    const std::string json = app::state().backup(includeSecrets);
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json.c_str());
    response->addHeader("Content-Disposition",
                        includeSecrets ? "attachment; filename=\"wikistats-backup-secrets.json\""
                                       : "attachment; filename=\"wikistats-backup.json\"");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    if (includeSecrets) {
        LOG_W(kTag, "configuration exported WITH secrets");
    }
}

void handleRestore(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    size_t length = 0;
    const char* body = bodyOf(request, length);
    if (body == nullptr) {
        sendError(request, 400, "missing body");
        return;
    }
    std::string error;
    if (!app::state().restore(body, length, error)) {
        sendError(request, 400, error.c_str());
        return;
    }
    requestTransportReload();
    JsonDocument out;
    out["ok"] = true;
    sendJson(request, 200, out);
}

void handleRestart(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    JsonDocument out;
    out["ok"] = true;
    out["note"] = "restarting";
    sendJson(request, 200, out);
    g_restartPending = true;  // main() reboots once the response has been flushed
}

void handleFactoryReset(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    size_t length = 0;
    const char* body = bodyOf(request, length);
    JsonDocument doc;
    if (body == nullptr || deserializeJson(doc, body, length) ||
        !(doc["confirm"] | false)) {
        // An accidental POST must not wipe a configured panel.
        sendError(request, 400, "send {\"confirm\":true} to erase all settings");
        return;
    }
    JsonDocument out;
    out["ok"] = true;
    out["note"] = "erasing configuration and restarting";
    sendJson(request, 200, out);
    g_factoryResetPending = true;
}

void handleFirmwareUploadFinished(AsyncWebServerRequest* request) {
    const bool ok = !Update.hasError();
    JsonDocument out;
    out["ok"] = ok;
    out["error"] = ok ? "" : Update.errorString();
    sendJson(request, ok ? 200 : 400, out);
    if (ok) {
        g_restartPending = true;
    }
}

void handleFirmwareUpload(AsyncWebServerRequest* request, const String& filename, size_t index,
                          uint8_t* data, size_t length, bool final) {
    if (index == 0) {
        if (!requireAuth(request, true)) {
            return;
        }
        LOG_W(kTag, "firmware upload started: %s", filename.c_str());
        // U_FLASH targets the inactive OTA slot; the running image is untouched
        // until the update verifies.
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            LOG_E(kTag, "update begin failed: %s", Update.errorString());
            return;
        }
    }
    if (Update.isRunning() && length > 0) {
        if (Update.write(data, length) != length) {
            LOG_E(kTag, "update write failed: %s", Update.errorString());
        }
    }
    if (final) {
        if (Update.end(true)) {
            LOG_W(kTag, "firmware upload complete (%u bytes)", (unsigned)(index + length));
        } else {
            LOG_E(kTag, "update end failed: %s", Update.errorString());
        }
    }
}

void handleNotFound(AsyncWebServerRequest* request) {
    if (request->method() == HTTP_OPTIONS) {
        request->send(200);
        return;
    }
    if (request->url().startsWith("/api/")) {
        sendError(request, 404, "no such endpoint");
        return;
    }
    if (wifi().portalActive()) {
        // Captive portal: any host the phone probes gets redirected to the setup UI,
        // which is what makes the "sign in to Wi-Fi" sheet appear.
        AsyncWebServerResponse* response = request->beginResponse(302);
        char location[48];
        snprintf(location, sizeof(location), "http://%s/", WiFi.softAPIP().toString().c_str());
        response->addHeader("Location", location);
        request->send(response);
        return;
    }
    request->send(404, "text/plain", "not found");
}

}  // namespace

// ------------------------------------------------------------- passwords

bool setAdminPassword(const std::string& password, std::string& errorOut) {
    if (password.size() < 8) {
        errorOut = "password must be at least 8 characters";
        return false;
    }
    if (password.size() > 64) {
        errorOut = "password must be at most 64 characters";
        return false;
    }
    uint8_t salt[kSaltBytes];
    for (size_t i = 0; i < kSaltBytes; ++i) {
        salt[i] = static_cast<uint8_t>(esp_random() & 0xFF);
    }
    uint8_t hash[kHashBytes];
    if (!pbkdf2(password, salt, kSaltBytes, hash)) {
        errorOut = "hashing failed";
        return false;
    }
    app::AppState& state = app::state();
    {
        app::AppState::Lock lock(state);
        state.config().web.passwordSalt = toHex(salt, kSaltBytes);
        state.config().web.passwordHash = toHex(hash, kHashBytes);
        state.config().web.passwordSet = true;
        state.touch();
    }
    return state.save();
}

bool verifyAdminPassword(const std::string& password) {
    std::string saltHex;
    std::string hashHex;
    {
        app::AppState& state = app::state();
        app::AppState::Lock lock(state);
        if (!state.config().web.passwordSet) {
            return false;
        }
        saltHex = state.config().web.passwordSalt;
        hashHex = state.config().web.passwordHash;
    }
    uint8_t salt[kSaltBytes];
    uint8_t expected[kHashBytes];
    if (!fromHex(saltHex, salt, kSaltBytes) || !fromHex(hashHex, expected, kHashBytes)) {
        return false;
    }
    uint8_t actual[kHashBytes];
    if (!pbkdf2(password, salt, kSaltBytes, actual)) {
        return false;
    }
    return secureEquals(reinterpret_cast<const char*>(actual),
                        reinterpret_cast<const char*>(expected), kHashBytes);
}

// ---------------------------------------------------------------- server

void startWebServer() {
    if (g_running) {
        return;
    }

    g_server.on("/api/status", HTTP_GET, handleStatus);
    g_server.on("/api/session", HTTP_GET, handleSession);
    g_server.on("/api/logs", HTTP_GET, handleLogs);
    g_server.on("/api/backup", HTTP_GET, handleBackup);
    g_server.on("/api/config", HTTP_GET, handleGetConfig);
    g_server.on("/api/devices", HTTP_GET, handleGetDevices);
    g_server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);

    g_server.on("/api/login", HTTP_POST, handleLogin, nullptr, collectBody);
    g_server.on("/api/logout", HTTP_POST, handleLogout);
    g_server.on("/api/password", HTTP_POST, handleSetPassword, nullptr, collectBody);
    g_server.on("/api/config", HTTP_PUT, handlePutConfig, nullptr, collectBody);
    g_server.on("/api/devices", HTTP_POST, handlePostDevice, nullptr, collectBody);
    g_server.on("/api/devices/order", HTTP_POST, handleReorder, nullptr, collectBody);
    g_server.on("/api/discovery/scan", HTTP_POST, handleDiscoveryScan);
    g_server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect, nullptr, collectBody);
    g_server.on("/api/wifi/forget", HTTP_POST, handleWifiForget, nullptr, collectBody);
    g_server.on("/api/wifi/forget-all", HTTP_POST, handleWifiForgetAll);
    g_server.on("/api/restore", HTTP_POST, handleRestore, nullptr, collectBody);
    g_server.on("/api/restart", HTTP_POST, handleRestart);
    g_server.on("/api/factory-reset", HTTP_POST, handleFactoryReset, nullptr, collectBody);
    g_server.on("/api/firmware", HTTP_POST, handleFirmwareUploadFinished, handleFirmwareUpload);

    // Item routes need regex; ASYNCWEBSERVER_REGEX is enabled in platformio.ini.
    g_server.on("^\\/api\\/devices\\/([^\\/]+)$", HTTP_PUT, handlePutDevice, nullptr, collectBody);
    g_server.on("^\\/api\\/devices\\/([^\\/]+)$", HTTP_DELETE, handleDeleteDevice);
    g_server.on("^\\/api\\/discovered\\/([^\\/]+)\\/approve$", HTTP_POST, handleApproveDevice);

    // Pre-compressed assets: serveStatic prefers "<path>.gz" and sets the header.
    g_server.serveStatic("/", LittleFS, "/www/")
        .setDefaultFile("index.html")
        .setCacheControl("max-age=86400");

    g_server.onNotFound(handleNotFound);
    g_server.begin();
    g_running = true;
    LOG_I(kTag, "web server listening on port %u", kPort);
}

void stopWebServer() {
    if (!g_running) {
        return;
    }
    g_server.end();
    g_running = false;
}

bool webServerRunning() { return g_running; }
bool restartPending() { return g_restartPending; }
bool factoryResetPending() { return g_factoryResetPending; }
void clearPendingActions() {
    g_restartPending = false;
    g_factoryResetPending = false;
}

}  // namespace net
