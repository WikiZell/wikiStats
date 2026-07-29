#include "fp_config.h"

#include <ArduinoJson.h>

#include <algorithm>

namespace fp {

namespace {

// --------------------------------------------------------------- readers
//
// Every reader is "leave the target alone unless the document supplies a usable
// value". That single rule gives patch semantics, forward compatibility with
// unknown keys, and immunity to a null or wrong-typed field, without a branch at
// each call site.

bool readString(JsonVariantConst source, std::string& target) {
    if (source.isNull() || !source.is<const char*>()) {
        return false;
    }
    const char* value = source.as<const char*>();
    if (value == nullptr) {
        return false;
    }
    // A redacted secret means "unchanged", never "empty".
    if (std::string(value) == kRedacted) {
        return false;
    }
    target.assign(value);
    return true;
}

bool readBool(JsonVariantConst source, bool& target) {
    if (source.isNull() || !source.is<bool>()) {
        return false;
    }
    target = source.as<bool>();
    return true;
}

template <typename T>
bool readNumber(JsonVariantConst source, T& target) {
    if (source.isNull() || !source.is<T>()) {
        return false;
    }
    target = source.as<T>();
    return true;
}

bool readFloat(JsonVariantConst source, float& target) {
    if (source.isNull() || !source.is<float>()) {
        return false;
    }
    target = source.as<float>();
    return true;
}

void writeSecret(JsonObject object, const char* key, const std::string& value,
                 SecretPolicy policy) {
    if (policy == SecretPolicy::Include) {
        object[key] = value;
        return;
    }
    // Redacted output still reports *whether* a secret is configured, so the UI can
    // show "password set" without ever transporting it.
    object[key] = value.empty() ? "" : kRedacted;
}

// --------------------------------------------------------------- migration

// Each step upgrades one version. Adding a v3 means adding one branch here and
// bumping kConfigVersion; stored documents from any earlier version still load.
void migrateV1ToV2(JsonObject root) {
    JsonObject carousel = root["carousel"];
    if (!carousel.isNull() && carousel["interval_ms"].is<uint32_t>()) {
        const uint32_t ms = carousel["interval_ms"].as<uint32_t>();
        carousel["interval_s"] = ms < 1000u ? 3u : ms / 1000u;
        carousel.remove("interval_ms");
    }

    JsonObject thresholds = root["thresholds"];
    if (!thresholds.isNull() && thresholds["cpu"].is<float>()) {
        // v1 stored one number per metric; v2 splits warning from critical.
        const float single = thresholds["cpu"].as<float>();
        thresholds["cpu_warn"] = single;
        thresholds["cpu_crit"] = std::min(100.0f, single + 15.0f);
        thresholds.remove("cpu");
    }

    JsonArray devices = root["devices"];
    if (!devices.isNull()) {
        for (JsonObject device : devices) {
            if (device["url"].is<const char*>() && !device["base_url"].is<const char*>()) {
                device["base_url"] = device["url"];
                device.remove("url");
            }
        }
    }
}

uint32_t migrate(JsonObject root) {
    uint32_t version = root["version"].is<uint32_t>() ? root["version"].as<uint32_t>() : 1u;
    const uint32_t original = version;
    while (version < kConfigVersion) {
        switch (version) {
            case 1:
                migrateV1ToV2(root);
                break;
            default:
                // No step defined: stop rather than loop forever.
                version = kConfigVersion;
                continue;
        }
        ++version;
    }
    root["version"] = kConfigVersion;
    return original;
}

// ------------------------------------------------------------------ apply

void applyDevices(JsonArrayConst array, PanelConfig& config) {
    std::vector<DeviceConfig> parsed;
    parsed.reserve(array.size() < kMaxDevices ? array.size() : kMaxDevices);
    for (JsonObjectConst item : array) {
        if (parsed.size() >= kMaxDevices) {
            break;
        }
        DeviceConfig device;
        if (!readString(item["id"], device.id) || device.id.empty()) {
            continue;  // a device without a stable ID cannot be merged or addressed
        }
        readString(item["name"], device.name);
        readString(item["alias"], device.alias);
        readString(item["base_url"], device.baseUrl);
        readString(item["path"], device.path);
        readString(item["platform"], device.platform);

        std::string sourceName = deviceSourceName(device.source);
        readString(item["source"], sourceName);
        device.source = deviceSourceFromName(sourceName);

        std::string authName = deviceAuthName(device.auth);
        readString(item["auth"], authName);
        device.auth = deviceAuthFromName(authName);

        // Preserve the stored token when the incoming document redacted it.
        const auto previous = std::find_if(
            config.devices.begin(), config.devices.end(),
            [&device](const DeviceConfig& existing) { return existing.id == device.id; });
        if (previous != config.devices.end()) {
            device.token = previous->token;
        }
        readString(item["token"], device.token);

        readBool(item["enabled"], device.enabled);
        readBool(item["hidden"], device.hidden);
        readBool(item["carousel"], device.carousel);
        readNumber<uint8_t>(item["order"], device.order);
        parsed.push_back(std::move(device));
    }
    config.devices = std::move(parsed);
}

void applyWifi(JsonObjectConst wifi, PanelConfig& config) {
    readString(wifi["hostname"], config.wifi.hostname);
    JsonArrayConst networks = wifi["networks"];
    if (networks.isNull()) {
        return;
    }
    std::vector<WifiNetwork> parsed;
    for (JsonObjectConst item : networks) {
        if (parsed.size() >= kMaxWifiNetworks) {
            break;
        }
        WifiNetwork network;
        if (!readString(item["ssid"], network.ssid) || network.ssid.empty()) {
            continue;
        }
        // Same redaction rule as tokens: an unchanged password must survive a PUT.
        const auto previous = std::find_if(
            config.wifi.networks.begin(), config.wifi.networks.end(),
            [&network](const WifiNetwork& existing) { return existing.ssid == network.ssid; });
        if (previous != config.wifi.networks.end()) {
            network.password = previous->password;
        }
        readString(item["password"], network.password);
        readNumber<uint8_t>(item["priority"], network.priority);
        readBool(item["hidden"], network.hidden);
        parsed.push_back(std::move(network));
    }
    config.wifi.networks = std::move(parsed);
}

void applyDocument(JsonObjectConst root, PanelConfig& config) {
    JsonObjectConst panel = root["panel"];
    if (!panel.isNull()) {
        readString(panel["name"], config.panel.name);
        readNumber<int16_t>(panel["timezone_offset_minutes"], config.panel.timezoneOffsetMinutes);
        std::string units = config.panel.binaryUnits ? "binary" : "decimal";
        if (readString(panel["units"], units)) {
            config.panel.binaryUnits = (units != "decimal");
        }
        std::string temperatureUnit = config.panel.fahrenheit ? "f" : "c";
        if (readString(panel["temperature_unit"], temperatureUnit)) {
            config.panel.fahrenheit = (temperatureUnit == "f");
        }
    }

    JsonObjectConst wifi = root["wifi"];
    if (!wifi.isNull()) {
        applyWifi(wifi, config);
    }

    JsonObjectConst mqtt = root["mqtt"];
    if (!mqtt.isNull()) {
        readBool(mqtt["enabled"], config.mqtt.enabled);
        readString(mqtt["host"], config.mqtt.host);
        readNumber<uint16_t>(mqtt["port"], config.mqtt.port);
        readString(mqtt["username"], config.mqtt.username);
        readString(mqtt["password"], config.mqtt.password);
        readBool(mqtt["tls"], config.mqtt.tls);
        readString(mqtt["base_topic"], config.mqtt.baseTopic);
        readString(mqtt["client_id"], config.mqtt.clientId);
    }

    JsonObjectConst transport = root["transport"];
    if (!transport.isNull()) {
        std::string mode = transportModeName(config.transport.mode);
        if (readString(transport["mode"], mode)) {
            config.transport.mode = transportModeFromName(mode);
        }
        readNumber<uint32_t>(transport["poll_interval_ms"], config.transport.pollIntervalMs);
        readNumber<uint32_t>(transport["http_timeout_ms"], config.transport.httpTimeoutMs);
        readNumber<uint32_t>(transport["max_payload_bytes"], config.transport.maxPayloadBytes);
        readNumber<uint32_t>(transport["mqtt_fresh_ms"], config.transport.mqttFreshMs);
    }

    JsonObjectConst discovery = root["discovery"];
    if (!discovery.isNull()) {
        readBool(discovery["mdns_enabled"], config.discovery.mdnsEnabled);
        readBool(discovery["auto_add"], config.discovery.autoAdd);
        readNumber<uint32_t>(discovery["interval_s"], config.discovery.intervalSeconds);
    }

    JsonObjectConst display = root["display"];
    if (!display.isNull()) {
        readNumber<uint8_t>(display["brightness"], config.display.brightness);
        readNumber<uint32_t>(display["screen_timeout_s"], config.display.screenTimeoutSeconds);
        readNumber<uint8_t>(display["dim_brightness"], config.display.dimBrightness);
        readNumber<uint8_t>(display["rotation"], config.display.rotation);
    }

    JsonObjectConst carousel = root["carousel"];
    if (!carousel.isNull()) {
        readBool(carousel["enabled"], config.carousel.enabled);
        readNumber<uint32_t>(carousel["interval_s"], config.carousel.intervalSeconds);
        readNumber<uint32_t>(carousel["idle_resume_s"], config.carousel.idleResumeSeconds);
        readBool(carousel["wrap"], config.carousel.wrap);
        readBool(carousel["include_offline"], config.carousel.includeOffline);
    }

    JsonObjectConst thresholds = root["thresholds"];
    if (!thresholds.isNull()) {
        readFloat(thresholds["cpu_warn"], config.thresholds.cpuWarn);
        readFloat(thresholds["cpu_crit"], config.thresholds.cpuCritical);
        readFloat(thresholds["cpu_temp_warn"], config.thresholds.cpuTempWarn);
        readFloat(thresholds["cpu_temp_crit"], config.thresholds.cpuTempCritical);
        readFloat(thresholds["ram_warn"], config.thresholds.ramWarn);
        readFloat(thresholds["ram_crit"], config.thresholds.ramCritical);
        readFloat(thresholds["disk_warn"], config.thresholds.diskWarn);
        readFloat(thresholds["disk_crit"], config.thresholds.diskCritical);
        readNumber<uint32_t>(thresholds["stale_s"], config.thresholds.staleSeconds);
        readNumber<uint32_t>(thresholds["offline_s"], config.thresholds.offlineSeconds);
    }

    JsonObjectConst web = root["web"];
    if (!web.isNull()) {
        readBool(web["enabled"], config.web.enabled);
        readString(web["username"], config.web.username);
        readString(web["password_hash"], config.web.passwordHash);
        readString(web["password_salt"], config.web.passwordSalt);
        readBool(web["password_set"], config.web.passwordSet);
        readNumber<uint32_t>(web["session_timeout_s"], config.web.sessionTimeoutSeconds);
    }

    JsonObjectConst logging = root["logging"];
    if (!logging.isNull()) {
        readString(logging["level"], config.logging.level);
        readBool(logging["telnet_enabled"], config.logging.telnetEnabled);
        readNumber<uint16_t>(logging["telnet_port"], config.logging.telnetPort);
    }

    JsonObjectConst ota = root["ota"];
    if (!ota.isNull()) {
        readBool(ota["arduino_ota_enabled"], config.ota.arduinoOtaEnabled);
        readString(ota["password"], config.ota.password);
    }

    JsonArrayConst devices = root["devices"];
    if (!devices.isNull()) {
        applyDevices(devices, config);
    }
}

}  // namespace

// ------------------------------------------------------------------- model

const char* transportModeName(TransportMode mode) {
    switch (mode) {
        case TransportMode::Http:
            return "http";
        case TransportMode::Mqtt:
            return "mqtt";
        case TransportMode::Auto:
        default:
            return "auto";
    }
}

TransportMode transportModeFromName(const std::string& name) {
    if (name == "http") {
        return TransportMode::Http;
    }
    if (name == "mqtt") {
        return TransportMode::Mqtt;
    }
    return TransportMode::Auto;
}

const char* configStatusName(ConfigStatus status) {
    switch (status) {
        case ConfigStatus::Ok:
            return "ok";
        case ConfigStatus::Defaults:
            return "defaults";
        case ConfigStatus::Migrated:
            return "migrated";
        case ConfigStatus::InvalidJson:
            return "invalid-json";
        case ConfigStatus::WrongSchema:
            return "wrong-schema";
        case ConfigStatus::TooNew:
            return "too-new";
    }
    return "unknown";
}

std::vector<const WifiNetwork*> WifiSettings::byPriority() const {
    std::vector<const WifiNetwork*> ordered;
    ordered.reserve(networks.size());
    for (const WifiNetwork& network : networks) {
        ordered.push_back(&network);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const WifiNetwork* a, const WifiNetwork* b) {
                         return a->priority < b->priority;
                     });
    return ordered;
}

bool WifiSettings::addOrReplace(const WifiNetwork& network) {
    if (network.ssid.empty()) {
        return false;
    }
    for (WifiNetwork& existing : networks) {
        if (existing.ssid == network.ssid) {
            existing = network;
            return true;
        }
    }
    if (networks.size() >= kMaxWifiNetworks) {
        return false;
    }
    networks.push_back(network);
    return true;
}

bool WifiSettings::forget(const std::string& ssid) {
    const size_t before = networks.size();
    networks.erase(std::remove_if(networks.begin(), networks.end(),
                                  [&ssid](const WifiNetwork& n) { return n.ssid == ssid; }),
                   networks.end());
    return networks.size() != before;
}

void WifiSettings::forgetAll() { networks.clear(); }

void PanelConfig::sanitise() {
    version = kConfigVersion;
    if (panel.name.empty()) {
        panel.name = "WikiStats";
    }
    if (panel.name.size() > 32) {
        panel.name.resize(32);
    }
    if (wifi.hostname.empty()) {
        wifi.hostname = "wikistats";
    }
    if (mqtt.port == 0) {
        mqtt.port = 1883;
    }
    if (mqtt.baseTopic.empty()) {
        mqtt.baseTopic = "fleetpanel/v1";
    }
    transport.pollIntervalMs = std::max<uint32_t>(1000, std::min<uint32_t>(300000, transport.pollIntervalMs));
    transport.httpTimeoutMs = std::max<uint32_t>(500, std::min<uint32_t>(30000, transport.httpTimeoutMs));
    transport.maxPayloadBytes =
        std::max<uint32_t>(1024, std::min<uint32_t>(65536, transport.maxPayloadBytes));
    transport.mqttFreshMs = std::max<uint32_t>(1000, std::min<uint32_t>(600000, transport.mqttFreshMs));
    discovery.intervalSeconds =
        std::max<uint32_t>(10, std::min<uint32_t>(3600, discovery.intervalSeconds));
    display.brightness = std::max<uint8_t>(5, std::min<uint8_t>(100, display.brightness));
    display.dimBrightness = std::min<uint8_t>(display.dimBrightness, display.brightness);
    display.rotation = display.rotation % 4;
    if (display.screenTimeoutSeconds > 3600) {
        display.screenTimeoutSeconds = 3600;
    }
    carousel.sanitise();
    thresholds.sanitise();
    if (web.username.empty()) {
        web.username = "admin";
    }
    web.sessionTimeoutSeconds =
        std::max<uint32_t>(60, std::min<uint32_t>(86400, web.sessionTimeoutSeconds));
    web.passwordSet = !web.passwordHash.empty() && !web.passwordSalt.empty();
    if (logging.telnetPort == 0) {
        logging.telnetPort = 23;
    }
    if (wifi.networks.size() > kMaxWifiNetworks) {
        wifi.networks.resize(kMaxWifiNetworks);
    }
    if (devices.size() > kMaxDevices) {
        devices.resize(kMaxDevices);
    }
    for (DeviceConfig& device : devices) {
        if (device.path.empty()) {
            device.path = "/api/v1/telemetry";
        }
    }
}

// ------------------------------------------------------------------ load

ConfigResult loadConfig(const char* json, size_t length, PanelConfig& out) {
    ConfigResult result;
    out = PanelConfig{};

    if (json == nullptr || length == 0) {
        out.sanitise();
        result.status = ConfigStatus::Defaults;
        result.message = "no stored configuration; using defaults";
        return result;
    }

    JsonDocument doc;
    const DeserializationError error =
        deserializeJson(doc, json, length, DeserializationOption::NestingLimit(12));
    if (error) {
        result.status = ConfigStatus::InvalidJson;
        result.message = error.c_str();
        out.sanitise();
        return result;
    }

    JsonObject root = doc.as<JsonObject>();
    if (root.isNull()) {
        result.status = ConfigStatus::InvalidJson;
        result.message = "root is not an object";
        out.sanitise();
        return result;
    }

    if (root["schema"].is<const char*>()) {
        const std::string schema = root["schema"].as<const char*>();
        if (schema != kConfigSchema) {
            result.status = ConfigStatus::WrongSchema;
            result.message = "unexpected schema: " + schema;
            out.sanitise();
            return result;
        }
    }

    const uint32_t storedVersion =
        root["version"].is<uint32_t>() ? root["version"].as<uint32_t>() : 1u;
    result.fromVersion = storedVersion;

    if (storedVersion > kConfigVersion) {
        // Written by newer firmware, most likely after a downgrade. Load what we
        // understand rather than throwing the user's settings away.
        applyDocument(root, out);
        out.sanitise();
        result.status = ConfigStatus::TooNew;
        result.message = "configuration was written by newer firmware; unknown keys ignored";
        return result;
    }

    const uint32_t original = migrate(root);
    applyDocument(root, out);
    out.sanitise();

    if (original < kConfigVersion) {
        result.status = ConfigStatus::Migrated;
        result.message = "migrated configuration from version " + std::to_string(original);
    } else {
        result.status = ConfigStatus::Ok;
    }
    return result;
}

ConfigResult mergeConfigPatch(const char* json, size_t length, PanelConfig& target) {
    ConfigResult result;
    if (json == nullptr || length == 0) {
        result.status = ConfigStatus::InvalidJson;
        result.message = "empty patch";
        return result;
    }

    JsonDocument doc;
    const DeserializationError error =
        deserializeJson(doc, json, length, DeserializationOption::NestingLimit(12));
    if (error) {
        result.status = ConfigStatus::InvalidJson;
        result.message = error.c_str();
        return result;
    }
    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull()) {
        result.status = ConfigStatus::InvalidJson;
        result.message = "root is not an object";
        return result;
    }

    applyDocument(root, target);
    target.sanitise();
    result.status = ConfigStatus::Ok;
    result.fromVersion = kConfigVersion;
    return result;
}

// ------------------------------------------------------------- serialise

std::string serialiseConfig(const PanelConfig& config, SecretPolicy policy) {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root["schema"] = kConfigSchema;
    root["version"] = kConfigVersion;

    JsonObject panel = root["panel"].to<JsonObject>();
    panel["name"] = config.panel.name;
    panel["timezone_offset_minutes"] = config.panel.timezoneOffsetMinutes;
    panel["units"] = config.panel.binaryUnits ? "binary" : "decimal";
    panel["temperature_unit"] = config.panel.fahrenheit ? "f" : "c";

    JsonObject wifi = root["wifi"].to<JsonObject>();
    wifi["hostname"] = config.wifi.hostname;
    JsonArray networks = wifi["networks"].to<JsonArray>();
    for (const WifiNetwork& network : config.wifi.networks) {
        JsonObject item = networks.add<JsonObject>();
        item["ssid"] = network.ssid;
        writeSecret(item, "password", network.password, policy);
        item["priority"] = network.priority;
        item["hidden"] = network.hidden;
    }

    JsonObject mqtt = root["mqtt"].to<JsonObject>();
    mqtt["enabled"] = config.mqtt.enabled;
    mqtt["host"] = config.mqtt.host;
    mqtt["port"] = config.mqtt.port;
    mqtt["username"] = config.mqtt.username;
    writeSecret(mqtt, "password", config.mqtt.password, policy);
    mqtt["tls"] = config.mqtt.tls;
    mqtt["base_topic"] = config.mqtt.baseTopic;
    mqtt["client_id"] = config.mqtt.clientId;

    JsonObject transport = root["transport"].to<JsonObject>();
    transport["mode"] = transportModeName(config.transport.mode);
    transport["poll_interval_ms"] = config.transport.pollIntervalMs;
    transport["http_timeout_ms"] = config.transport.httpTimeoutMs;
    transport["max_payload_bytes"] = config.transport.maxPayloadBytes;
    transport["mqtt_fresh_ms"] = config.transport.mqttFreshMs;

    JsonObject discovery = root["discovery"].to<JsonObject>();
    discovery["mdns_enabled"] = config.discovery.mdnsEnabled;
    discovery["auto_add"] = config.discovery.autoAdd;
    discovery["interval_s"] = config.discovery.intervalSeconds;

    JsonObject display = root["display"].to<JsonObject>();
    display["brightness"] = config.display.brightness;
    display["screen_timeout_s"] = config.display.screenTimeoutSeconds;
    display["dim_brightness"] = config.display.dimBrightness;
    display["rotation"] = config.display.rotation;

    JsonObject carousel = root["carousel"].to<JsonObject>();
    carousel["enabled"] = config.carousel.enabled;
    carousel["interval_s"] = config.carousel.intervalSeconds;
    carousel["idle_resume_s"] = config.carousel.idleResumeSeconds;
    carousel["wrap"] = config.carousel.wrap;
    carousel["include_offline"] = config.carousel.includeOffline;

    JsonObject thresholds = root["thresholds"].to<JsonObject>();
    thresholds["cpu_warn"] = config.thresholds.cpuWarn;
    thresholds["cpu_crit"] = config.thresholds.cpuCritical;
    thresholds["cpu_temp_warn"] = config.thresholds.cpuTempWarn;
    thresholds["cpu_temp_crit"] = config.thresholds.cpuTempCritical;
    thresholds["ram_warn"] = config.thresholds.ramWarn;
    thresholds["ram_crit"] = config.thresholds.ramCritical;
    thresholds["disk_warn"] = config.thresholds.diskWarn;
    thresholds["disk_crit"] = config.thresholds.diskCritical;
    thresholds["stale_s"] = config.thresholds.staleSeconds;
    thresholds["offline_s"] = config.thresholds.offlineSeconds;

    JsonObject web = root["web"].to<JsonObject>();
    web["enabled"] = config.web.enabled;
    web["username"] = config.web.username;
    writeSecret(web, "password_hash", config.web.passwordHash, policy);
    writeSecret(web, "password_salt", config.web.passwordSalt, policy);
    web["password_set"] = config.web.passwordSet;
    web["session_timeout_s"] = config.web.sessionTimeoutSeconds;

    JsonObject logging = root["logging"].to<JsonObject>();
    logging["level"] = config.logging.level;
    logging["telnet_enabled"] = config.logging.telnetEnabled;
    logging["telnet_port"] = config.logging.telnetPort;

    JsonObject ota = root["ota"].to<JsonObject>();
    ota["arduino_ota_enabled"] = config.ota.arduinoOtaEnabled;
    writeSecret(ota, "password", config.ota.password, policy);

    JsonArray devices = root["devices"].to<JsonArray>();
    for (const DeviceConfig& device : config.devices) {
        JsonObject item = devices.add<JsonObject>();
        item["id"] = device.id;
        item["name"] = device.name;
        item["alias"] = device.alias;
        item["base_url"] = device.baseUrl;
        item["path"] = device.path;
        item["platform"] = device.platform;
        item["source"] = deviceSourceName(device.source);
        item["auth"] = deviceAuthName(device.auth);
        writeSecret(item, "token", device.token, policy);
        item["enabled"] = device.enabled;
        item["hidden"] = device.hidden;
        item["carousel"] = device.carousel;
        item["order"] = device.order;
    }

    std::string out;
    serializeJson(doc, out);
    return out;
}

}  // namespace fp
