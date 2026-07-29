// Shared application state.
//
// Three tasks touch this: the UI task (LVGL), the network task (HTTP/MQTT/mDNS) and
// the AsyncTCP task behind the web server. Every one of them goes through the same
// recursive mutex, and every mutation bumps `revision()` so the UI can repaint only
// when something actually changed - which is what makes a setting altered in the
// browser appear on the touchscreen immediately, and vice versa.
//
// Persistence is atomic. `save()` writes config.tmp, re-reads and re-parses it, and
// only then rotates config.json to config.bak and config.tmp into place. Power loss
// at any point leaves at least one complete, parseable document on flash.
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string>

#include "fp_config.h"
#include "fp_devices.h"

namespace app {

constexpr const char* kConfigPath = "/config.json";
constexpr const char* kBackupPath = "/config.bak";
constexpr const char* kTempPath = "/config.tmp";

enum class WifiPhase : uint8_t {
    Booting = 0,
    Connecting,
    Connected,
    Portal,   // access point + captive portal, no station connection
    Failed,
};

const char* wifiPhaseName(WifiPhase phase);

struct RuntimeStatus {
    WifiPhase wifi = WifiPhase::Booting;
    char ssid[33] = {0};
    char ip[16] = {0};
    char apSsid[33] = {0};
    char apIp[16] = {0};
    int8_t rssi = 0;
    bool mqttConnected = false;
    bool mdnsRunning = false;
    uint32_t discoveredCount = 0;
    uint32_t httpPolls = 0;
    uint32_t httpFailures = 0;
    uint32_t mqttMessages = 0;
    uint32_t lastMqttSampleMs = 0;
    uint32_t bootCount = 0;
    char resetReason[24] = {0};
    char configStatus[48] = {0};
};

class AppState {
   public:
    static AppState& instance();

    // Mounts LittleFS (formatting it only if it has never been formatted), loads
    // and migrates the configuration, and records the reset reason.
    bool begin();

    // RAII lock. The mutex is recursive so a locked helper may call another.
    class Lock {
       public:
        explicit Lock(AppState& state) : state_(state) { state_.take(); }
        ~Lock() { state_.give(); }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

       private:
        AppState& state_;
    };

    void take();
    void give();

    fp::PanelConfig& config() { return config_; }
    const fp::PanelConfig& config() const { return config_; }
    fp::DeviceRegistry& devices() { return devices_; }
    const fp::DeviceRegistry& devices() const { return devices_; }
    RuntimeStatus& status() { return status_; }
    const RuntimeStatus& status() const { return status_; }

    // Call after any mutation. Cheap; it only increments a counter.
    void touch() { ++revision_; }
    uint32_t revision() const { return revision_; }

    // Persists the current configuration. Returns false and leaves the previous
    // document intact if the write or the verification round-trip fails.
    bool save();

    // Marks the configuration as needing a write. The UI task flushes it a moment
    // later, which coalesces a burst of slider movements into one flash write.
    void requestSave() { saveRequestedAtMs_ = millis() ? millis() : 1; }
    bool flushPendingSave(uint32_t nowMs, uint32_t settleMs = 1500);

    // JSON backup/restore. Secrets are included only on explicit request.
    std::string backup(bool includeSecrets) const;
    bool restore(const char* json, size_t length, std::string& errorOut);

    // Wipes configuration and both stored copies, then reboots the caller's choice.
    bool factoryReset();

    // Rebuilds the device registry from config.devices and vice versa.
    void syncRegistryFromConfig();
    void syncConfigFromRegistry();

    const char* lastConfigStatus() const { return status_.configStatus; }
    bool storageReady() const { return storageReady_; }
    uint32_t chipId() const { return chipId_; }
    const char* defaultHostname() const { return defaultHostname_; }

   private:
    AppState() = default;
    bool load();
    bool writeFileAtomic(const char* path, const std::string& contents);
    bool readFile(const char* path, std::string& out) const;

    SemaphoreHandle_t mutex_ = nullptr;
    fp::PanelConfig config_;
    fp::DeviceRegistry devices_;
    RuntimeStatus status_;
    volatile uint32_t revision_ = 0;
    uint32_t saveRequestedAtMs_ = 0;
    bool storageReady_ = false;
    uint32_t chipId_ = 0;
    char defaultHostname_[24] = {0};
};

inline AppState& state() { return AppState::instance(); }

}  // namespace app
