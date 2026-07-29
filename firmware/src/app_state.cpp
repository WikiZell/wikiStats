#include "app_state.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <esp_system.h>

#include "log.h"

namespace app {

namespace {

constexpr const char* kTag = "state";
constexpr size_t kMaxConfigBytes = 24 * 1024;

const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "power-on";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "int-watchdog";
        case ESP_RST_TASK_WDT:
            return "task-watchdog";
        case ESP_RST_WDT:
            return "watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep-sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_UNKNOWN:
        default:
            return "unknown";
    }
}

}  // namespace

const char* wifiPhaseName(WifiPhase phase) {
    switch (phase) {
        case WifiPhase::Connecting:
            return "connecting";
        case WifiPhase::Connected:
            return "connected";
        case WifiPhase::Portal:
            return "portal";
        case WifiPhase::Failed:
            return "failed";
        case WifiPhase::Booting:
        default:
            return "booting";
    }
}

AppState& AppState::instance() {
    static AppState singleton;
    return singleton;
}

void AppState::take() {
    if (mutex_ != nullptr) {
        xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
    }
}

void AppState::give() {
    if (mutex_ != nullptr) {
        xSemaphoreGiveRecursive(mutex_);
    }
}

bool AppState::begin() {
    mutex_ = xSemaphoreCreateRecursiveMutex();

    uint64_t mac = ESP.getEfuseMac();
    chipId_ = static_cast<uint32_t>(mac >> 24);
    snprintf(defaultHostname_, sizeof(defaultHostname_), "wikistats-%04x",
             static_cast<uint16_t>(chipId_ & 0xFFFF));

    const esp_reset_reason_t reason = esp_reset_reason();
    snprintf(status_.resetReason, sizeof(status_.resetReason), "%s", resetReasonName(reason));

    // A boot counter in NVS survives a filesystem wipe, which makes it the one
    // number that can distinguish "first boot ever" from "reset loop".
    Preferences prefs;
    if (prefs.begin("wikistats", false)) {
        status_.bootCount = prefs.getUInt("boots", 0) + 1;
        prefs.putUInt("boots", status_.bootCount);
        prefs.end();
    }
    LOG_I(kTag, "boot #%lu, reset reason: %s", (unsigned long)status_.bootCount,
          status_.resetReason);

    // The partition is labelled "littlefs" in partitions_wikistats.csv, not the
    // Arduino default of "spiffs", so the label has to be passed explicitly or the
    // mount fails with `partition "spiffs" could not be found`.
    // `true` formats only when no valid filesystem is present, not on every failure.
    storageReady_ = LittleFS.begin(true, "/littlefs", 10, "littlefs");
    if (!storageReady_) {
        LOG_E(kTag, "LittleFS mount failed; running with defaults, settings will not persist");
        config_ = fp::PanelConfig{};
        config_.sanitise();
    } else {
        load();
    }

    if (config_.wifi.hostname.empty() || config_.wifi.hostname == "wikistats") {
        config_.wifi.hostname = defaultHostname_;
    }
    syncRegistryFromConfig();
    return storageReady_;
}

bool AppState::readFile(const char* path, std::string& out) const {
    out.clear();
    File file = LittleFS.open(path, "r");
    if (!file) {
        return false;
    }
    const size_t size = file.size();
    if (size == 0 || size > kMaxConfigBytes) {
        file.close();
        return false;
    }
    out.resize(size);
    const size_t read = file.readBytes(&out[0], size);
    file.close();
    out.resize(read);
    return read > 0;
}

bool AppState::load() {
    std::string raw;
    fp::ConfigResult result;

    if (readFile(kConfigPath, raw)) {
        result = fp::loadConfig(raw, config_);
        if (!result.usable()) {
            LOG_W(kTag, "primary config unusable (%s); trying backup",
                  fp::configStatusName(result.status));
            if (readFile(kBackupPath, raw)) {
                result = fp::loadConfig(raw, config_);
            }
        }
    } else if (readFile(kBackupPath, raw)) {
        LOG_W(kTag, "primary config missing; loaded backup");
        result = fp::loadConfig(raw, config_);
    } else {
        result = fp::loadConfig(nullptr, 0, config_);
    }

    if (!result.usable()) {
        LOG_E(kTag, "no usable configuration (%s); falling back to defaults",
              fp::configStatusName(result.status));
        config_ = fp::PanelConfig{};
        config_.sanitise();
    }

    snprintf(status_.configStatus, sizeof(status_.configStatus), "%s%s%s",
             fp::configStatusName(result.status), result.message.empty() ? "" : ": ",
             result.message.c_str());
    LOG_I(kTag, "config %s (from v%lu)", status_.configStatus, (unsigned long)result.fromVersion);

    if (result.status == fp::ConfigStatus::Migrated) {
        // Persist the upgraded document now so the migration runs once, not on
        // every boot.
        save();
    }
    return result.usable();
}

bool AppState::writeFileAtomic(const char* path, const std::string& contents) {
    File file = LittleFS.open(kTempPath, "w");
    if (!file) {
        LOG_E(kTag, "cannot open %s for writing", kTempPath);
        return false;
    }
    const size_t written = file.print(contents.c_str());
    file.flush();
    file.close();
    if (written != contents.size()) {
        LOG_E(kTag, "short write: %u of %u bytes", (unsigned)written, (unsigned)contents.size());
        LittleFS.remove(kTempPath);
        return false;
    }

    // Read the temp file back and parse it before letting it replace the live copy.
    // A truncated or corrupted write must never become the only document on flash.
    std::string verify;
    if (!readFile(kTempPath, verify) || verify.size() != contents.size()) {
        LOG_E(kTag, "verification read failed; keeping previous configuration");
        LittleFS.remove(kTempPath);
        return false;
    }
    fp::PanelConfig probe;
    if (!fp::loadConfig(verify, probe).usable()) {
        LOG_E(kTag, "written configuration does not parse; keeping previous configuration");
        LittleFS.remove(kTempPath);
        return false;
    }

    if (LittleFS.exists(kBackupPath)) {
        LittleFS.remove(kBackupPath);
    }
    if (LittleFS.exists(path)) {
        LittleFS.rename(path, kBackupPath);
    }
    if (!LittleFS.rename(kTempPath, path)) {
        LOG_E(kTag, "rename failed; restoring backup");
        LittleFS.rename(kBackupPath, path);
        return false;
    }
    return true;
}

bool AppState::save() {
    if (!storageReady_) {
        return false;
    }
    Lock lock(*this);
    syncConfigFromRegistry();
    config_.sanitise();
    const std::string json = fp::serialiseConfig(config_, fp::SecretPolicy::Include);
    if (json.size() > kMaxConfigBytes) {
        LOG_E(kTag, "configuration is %u bytes, over the %u limit", (unsigned)json.size(),
              (unsigned)kMaxConfigBytes);
        return false;
    }
    const bool ok = writeFileAtomic(kConfigPath, json);
    LOG_I(kTag, "configuration %s (%u bytes)", ok ? "saved" : "SAVE FAILED", (unsigned)json.size());
    saveRequestedAtMs_ = 0;
    return ok;
}

bool AppState::flushPendingSave(uint32_t nowMs, uint32_t settleMs) {
    if (saveRequestedAtMs_ == 0) {
        return false;
    }
    if (nowMs - saveRequestedAtMs_ < settleMs) {
        return false;
    }
    return save();
}

std::string AppState::backup(bool includeSecrets) const {
    // const_cast: the lock is a mutex, not part of the logical state.
    AppState& self = const_cast<AppState&>(*this);
    Lock lock(self);
    self.syncConfigFromRegistry();
    return fp::serialiseConfig(config_,
                               includeSecrets ? fp::SecretPolicy::Include
                                              : fp::SecretPolicy::Redact);
}

bool AppState::restore(const char* json, size_t length, std::string& errorOut) {
    Lock lock(*this);
    fp::PanelConfig candidate;
    const fp::ConfigResult result = fp::loadConfig(json, length, candidate);
    if (!result.usable()) {
        errorOut = result.message.empty() ? fp::configStatusName(result.status) : result.message;
        return false;
    }
    // A plain (redacted) backup carries no secrets, so keep the ones already on the
    // device rather than locking the user out of their own broker and Wi-Fi.
    if (candidate.web.passwordHash.empty()) {
        candidate.web.passwordHash = config_.web.passwordHash;
        candidate.web.passwordSalt = config_.web.passwordSalt;
    }
    for (fp::WifiNetwork& network : candidate.wifi.networks) {
        if (!network.password.empty()) {
            continue;
        }
        for (const fp::WifiNetwork& existing : config_.wifi.networks) {
            if (existing.ssid == network.ssid) {
                network.password = existing.password;
                break;
            }
        }
    }
    if (candidate.mqtt.password.empty()) {
        candidate.mqtt.password = config_.mqtt.password;
    }

    config_ = candidate;
    config_.sanitise();
    syncRegistryFromConfig();
    touch();
    return save();
}

bool AppState::factoryReset() {
    Lock lock(*this);
    LittleFS.remove(kConfigPath);
    LittleFS.remove(kBackupPath);
    LittleFS.remove(kTempPath);
    Preferences prefs;
    if (prefs.begin("wikistats", false)) {
        prefs.clear();
        prefs.end();
    }
    config_ = fp::PanelConfig{};
    config_.wifi.hostname = defaultHostname_;
    config_.sanitise();
    devices_.loadConfigs({});
    devices_.clearPending();
    touch();
    LOG_W(kTag, "factory reset complete");
    return true;
}

void AppState::syncRegistryFromConfig() {
    devices_.loadConfigs(config_.devices);
}

void AppState::syncConfigFromRegistry() {
    config_.devices = devices_.exportConfigs();
}

}  // namespace app
