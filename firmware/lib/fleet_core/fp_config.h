// Versioned panel configuration: model, JSON round-trip, migration, redaction.
//
// One model backs both user interfaces. The touchscreen and the web UI mutate the
// same `PanelConfig` and persist through the same serialiser, which is what makes a
// change on one appear immediately on the other.
//
// Secrets (Wi-Fi passwords, the MQTT password, per-device API tokens, the web
// password hash and salt, the OTA password) are written to flash but replaced with
// `kRedacted` by `SecretPolicy::Redact`. `GET /api/config` and the plain backup both
// use Redact; only an explicitly requested secret-bearing backup uses Include.
//
// `mergeConfigPatch` exists for the same reason: a client that received a redacted
// document must be able to PUT it back without wiping the credentials it never saw.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fp_carousel.h"
#include "fp_devices.h"
#include "fp_thresholds.h"

namespace fp {

// Bumped whenever the on-flash layout changes. `migrate()` walks a stored document
// forward one version at a time.
constexpr uint32_t kConfigVersion = 2;
constexpr const char* kConfigSchema = "fleetpanel.config.v1";
constexpr const char* kRedacted = "__redacted__";
constexpr size_t kMaxWifiNetworks = 8;

struct WifiNetwork {
    std::string ssid;
    std::string password;
    uint8_t priority = 0;  // lower value tried first
    bool hidden = false;
};

struct PanelSettings {
    std::string name = "WikiStats";
    int16_t timezoneOffsetMinutes = 0;
    bool binaryUnits = true;   // KiB/MiB/GiB rather than kB/MB/GB
    bool fahrenheit = false;
};

struct WifiSettings {
    std::string hostname = "wikistats";
    std::vector<WifiNetwork> networks;

    // Networks sorted by priority then insertion order.
    std::vector<const WifiNetwork*> byPriority() const;
    bool addOrReplace(const WifiNetwork& network);
    bool forget(const std::string& ssid);
    void forgetAll();
};

struct MqttSettings {
    bool enabled = false;
    std::string host;
    uint16_t port = 1883;
    std::string username;
    std::string password;
    bool tls = false;
    std::string baseTopic = "fleetpanel/v1";
    std::string clientId;  // empty = derive from the chip ID
};

enum class TransportMode : uint8_t {
    Http = 0,
    Mqtt,
    Auto,
};

const char* transportModeName(TransportMode mode);
TransportMode transportModeFromName(const std::string& name);

struct TransportSettings {
    TransportMode mode = TransportMode::Auto;
    uint32_t pollIntervalMs = 3000;
    uint32_t httpTimeoutMs = 4000;
    uint32_t maxPayloadBytes = 16384;
    // In Auto mode, how recent an MQTT sample must be to keep HTTP polling off.
    uint32_t mqttFreshMs = 20000;
};

struct DiscoverySettings {
    bool mdnsEnabled = true;
    bool autoAdd = false;  // otherwise discoveries wait in the pending list
    uint32_t intervalSeconds = 60;
};

struct DisplaySettings {
    uint8_t brightness = 80;
    uint32_t screenTimeoutSeconds = 0;  // 0 = never dim
    uint8_t dimBrightness = 10;
    uint8_t rotation = 1;  // 1 = landscape, USB on the left
};

struct WebSettings {
    bool enabled = true;
    std::string username = "admin";
    std::string passwordHash;
    std::string passwordSalt;
    bool passwordSet = false;
    uint32_t sessionTimeoutSeconds = 3600;
};

struct LoggingSettings {
    std::string level = "info";
    bool telnetEnabled = true;  // remote serial console over Wi-Fi
    uint16_t telnetPort = 23;
};

struct OtaSettings {
    bool arduinoOtaEnabled = true;
    std::string password;  // empty = fall back to the web admin password
};

struct PanelConfig {
    uint32_t version = kConfigVersion;
    PanelSettings panel;
    WifiSettings wifi;
    MqttSettings mqtt;
    TransportSettings transport;
    DiscoverySettings discovery;
    DisplaySettings display;
    CarouselSettings carousel;
    Thresholds thresholds;
    WebSettings web;
    LoggingSettings logging;
    OtaSettings ota;
    std::vector<DeviceConfig> devices;

    void sanitise();
};

enum class ConfigStatus : uint8_t {
    Ok = 0,
    Defaults,      // no stored document; a fresh install
    Migrated,      // loaded and upgraded from an older version
    InvalidJson,   // unparseable; caller should fall back to the backup copy
    WrongSchema,   // parseable but not ours
    TooNew,        // written by newer firmware; loaded best-effort
};

struct ConfigResult {
    ConfigStatus status = ConfigStatus::Defaults;
    uint32_t fromVersion = 0;
    std::string message;

    bool usable() const {
        return status == ConfigStatus::Ok || status == ConfigStatus::Defaults ||
               status == ConfigStatus::Migrated || status == ConfigStatus::TooNew;
    }
};

const char* configStatusName(ConfigStatus status);

enum class SecretPolicy : uint8_t {
    Redact = 0,
    Include,
};

// Parses, migrates and validates. `out` is left at defaults on a hard failure.
ConfigResult loadConfig(const char* json, size_t length, PanelConfig& out);
inline ConfigResult loadConfig(const std::string& json, PanelConfig& out) {
    return loadConfig(json.c_str(), json.size(), out);
}

std::string serialiseConfig(const PanelConfig& config, SecretPolicy policy);

// Applies a partial document on top of `target`. Absent keys are left alone and
// values equal to `kRedacted` are ignored, so a client can PUT back a document it
// received redacted without destroying credentials.
ConfigResult mergeConfigPatch(const char* json, size_t length, PanelConfig& target);
inline ConfigResult mergeConfigPatch(const std::string& json, PanelConfig& target) {
    return mergeConfigPatch(json.c_str(), json.size(), target);
}

}  // namespace fp
