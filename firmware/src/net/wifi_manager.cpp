#include "wifi_manager.h"

#include <DNSServer.h>
#include <WiFi.h>

#include <algorithm>

#include "../app_state.h"
#include "../log.h"

namespace net {

namespace {

constexpr const char* kTag = "wifi";
constexpr uint32_t kConnectTimeoutMs = 12000;
constexpr uint32_t kMaxBackoffMs = 60000;
constexpr uint32_t kStatusPushIntervalMs = 2000;
constexpr uint8_t kPortalChannel = 1;
constexpr size_t kMaxScanEntries = 24;

DNSServer g_dns;
bool g_dnsRunning = false;

}  // namespace

WifiManager& wifi() {
    static WifiManager instance;
    return instance;
}

void WifiManager::applyHostname() {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    const std::string& hostname = state.config().wifi.hostname;
    WiFi.setHostname(hostname.c_str());
}

void WifiManager::begin() {
    WiFi.persistent(false);          // credentials live in our config, not in NVS
    WiFi.setAutoReconnect(false);    // reconnection is driven here, with backoff
    WiFi.mode(WIFI_STA);
    applyHostname();

    size_t saved = 0;
    {
        app::AppState& state = app::state();
        app::AppState::Lock lock(state);
        saved = state.config().wifi.networks.size();
        state.status().wifi = app::WifiPhase::Booting;
    }

    if (saved == 0) {
        startPortal("no saved networks");
        return;
    }
    if (forcePortal_) {
        // Recovery path: the portal comes up alongside the station interface, so a
        // working network keeps working while the user adds a new one.
        startPortal("touchscreen held during boot");
    }
    nextNetworkIndex_ = 0;
    nextAttemptMs_ = 0;
    snprintf(message_, sizeof(message_), "%u saved network%s", (unsigned)saved,
             saved == 1 ? "" : "s");
}

void WifiManager::startPortal(const char* reason) {
    if (portalActive_) {
        return;
    }
    app::AppState& state = app::state();
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "FleetPanel-%04X", static_cast<uint16_t>(state.chipId() & 0xFFFF));

    WiFi.mode(WIFI_AP_STA);
    // Open network on purpose: a WPA password on a setup portal is one more thing to
    // read off a 320x240 screen, and the portal exposes only Wi-Fi provisioning.
    WiFi.softAP(ssid, nullptr, kPortalChannel, 0, 4);
    delay(100);
    const IPAddress ip = WiFi.softAPIP();

    g_dns.setErrorReplyCode(DNSReplyCode::NoError);
    // Wildcard: every lookup resolves to the panel, which is what makes phones pop
    // up the "sign in to network" sheet.
    g_dnsRunning = g_dns.start(53, "*", ip);

    portalActive_ = true;
    connecting_ = false;
    snprintf(message_, sizeof(message_), "setup portal: %s", reason);

    {
        app::AppState::Lock lock(state);
        app::RuntimeStatus& status = state.status();
        status.wifi = app::WifiPhase::Portal;
        snprintf(status.apSsid, sizeof(status.apSsid), "%s", ssid);
        snprintf(status.apIp, sizeof(status.apIp), "%s", ip.toString().c_str());
        state.touch();
    }
    LOG_W(kTag, "setup portal up: SSID %s, http://%s (%s)", ssid, ip.toString().c_str(), reason);
}

void WifiManager::stopPortal() {
    if (!portalActive_) {
        return;
    }
    if (g_dnsRunning) {
        g_dns.stop();
        g_dnsRunning = false;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    portalActive_ = false;
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    state.status().apSsid[0] = '\0';
    state.status().apIp[0] = '\0';
    state.touch();
    LOG_I(kTag, "setup portal stopped");
}

void WifiManager::requestScan() {
    if (scanRunning_) {
        return;
    }
    WiFi.scanDelete();
    // Async scan: returns immediately, result collected in loop().
    const int16_t started = WiFi.scanNetworks(true, true);
    scanRunning_ = started == WIFI_SCAN_RUNNING || started >= 0;
    scanReady_ = false;
    if (!scanRunning_) {
        LOG_W(kTag, "scan could not be started");
    }
}

bool WifiManager::takeScanResult() {
    if (!scanReady_) {
        return false;
    }
    scanReady_ = false;
    return true;
}

void WifiManager::markKnownNetworks() {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    for (ScanEntry& entry : scanResults_) {
        entry.known = false;
        for (const fp::WifiNetwork& saved : state.config().wifi.networks) {
            if (saved.ssid == entry.ssid) {
                entry.known = true;
                break;
            }
        }
    }
}

bool WifiManager::connectTo(const std::string& ssid, const std::string& password, bool remember,
                            bool hidden) {
    if (ssid.empty()) {
        snprintf(message_, sizeof(message_), "SSID cannot be empty");
        return false;
    }
    if (remember) {
        app::AppState& state = app::state();
        app::AppState::Lock lock(state);
        fp::WifiNetwork network;
        network.ssid = ssid;
        network.password = password;
        network.hidden = hidden;
        // New networks go to the front of the priority list: the user just chose it,
        // so it is what they want the panel on.
        for (fp::WifiNetwork& existing : state.config().wifi.networks) {
            if (existing.priority < 254) {
                ++existing.priority;
            }
        }
        network.priority = 0;
        if (!state.config().wifi.addOrReplace(network)) {
            snprintf(message_, sizeof(message_), "saved network list is full (max %u)",
                     (unsigned)fp::kMaxWifiNetworks);
            return false;
        }
        state.touch();
        state.requestSave();
    }

    WiFi.disconnect(false, false);
    if (!portalActive_) {
        WiFi.mode(WIFI_STA);
    }
    applyHostname();
    connectingSsid_ = ssid;
    connecting_ = true;
    attemptStartedMs_ = millis();
    failures_ = 0;
    backoffMs_ = 2000;
    WiFi.begin(ssid.c_str(), password.empty() ? nullptr : password.c_str());
    snprintf(message_, sizeof(message_), "connecting to %s", ssid.c_str());

    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    state.status().wifi = app::WifiPhase::Connecting;
    snprintf(state.status().ssid, sizeof(state.status().ssid), "%s", ssid.c_str());
    state.touch();
    LOG_I(kTag, "connecting to %s", ssid.c_str());
    return true;
}

void WifiManager::reconnectNow() {
    nextAttemptMs_ = 0;
    backoffMs_ = 2000;
    connecting_ = false;
    WiFi.disconnect(false, false);
}

void WifiManager::forget(const std::string& ssid) {
    app::AppState& state = app::state();
    {
        app::AppState::Lock lock(state);
        if (state.config().wifi.forget(ssid)) {
            state.touch();
            state.requestSave();
            LOG_I(kTag, "forgot network %s", ssid.c_str());
        }
    }
    if (WiFi.SSID() == ssid.c_str()) {
        WiFi.disconnect(false, false);
        connecting_ = false;
        nextAttemptMs_ = 0;
    }
}

void WifiManager::forgetAll() {
    app::AppState& state = app::state();
    {
        app::AppState::Lock lock(state);
        state.config().wifi.forgetAll();
        state.touch();
        state.requestSave();
    }
    WiFi.disconnect(false, false);
    connecting_ = false;
    LOG_W(kTag, "all saved networks forgotten");
    startPortal("all networks forgotten");
}

bool WifiManager::connected() const { return WiFi.status() == WL_CONNECTED; }

bool WifiManager::startConnectAttempt(uint32_t nowMs) {
    std::string ssid;
    std::string password;
    size_t count = 0;
    {
        app::AppState& state = app::state();
        app::AppState::Lock lock(state);
        const std::vector<const fp::WifiNetwork*> ordered = state.config().wifi.byPriority();
        count = ordered.size();
        if (count == 0) {
            return false;
        }
        const fp::WifiNetwork* chosen = ordered[nextNetworkIndex_ % count];
        ssid = chosen->ssid;
        password = chosen->password;
    }
    nextNetworkIndex_ = (nextNetworkIndex_ + 1) % count;

    WiFi.disconnect(false, false);
    applyHostname();
    WiFi.begin(ssid.c_str(), password.empty() ? nullptr : password.c_str());
    connectingSsid_ = ssid;
    connecting_ = true;
    attemptStartedMs_ = nowMs;
    snprintf(message_, sizeof(message_), "connecting to %s", ssid.c_str());

    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    state.status().wifi = app::WifiPhase::Connecting;
    snprintf(state.status().ssid, sizeof(state.status().ssid), "%s", ssid.c_str());
    state.touch();
    return true;
}

void WifiManager::onConnected() {
    connecting_ = false;
    failures_ = 0;
    backoffMs_ = 2000;
    const IPAddress ip = WiFi.localIP();
    snprintf(message_, sizeof(message_), "connected to %s", WiFi.SSID().c_str());

    app::AppState& state = app::state();
    {
        app::AppState::Lock lock(state);
        app::RuntimeStatus& status = state.status();
        status.wifi = app::WifiPhase::Connected;
        snprintf(status.ssid, sizeof(status.ssid), "%s", WiFi.SSID().c_str());
        snprintf(status.ip, sizeof(status.ip), "%s", ip.toString().c_str());
        status.rssi = static_cast<int8_t>(WiFi.RSSI());
        state.touch();
    }
    LOG_I(kTag, "connected to %s as %s (%d dBm)", WiFi.SSID().c_str(), ip.toString().c_str(),
          WiFi.RSSI());

    // Once there is an address, the setup portal has served its purpose.
    if (portalActive_) {
        stopPortal();
    }
}

void WifiManager::onFailed(uint32_t nowMs, const char* why) {
    connecting_ = false;
    ++failures_;
    nextAttemptMs_ = nowMs + backoffMs_;
    backoffMs_ = backoffMs_ >= kMaxBackoffMs ? kMaxBackoffMs : backoffMs_ * 2;
    snprintf(message_, sizeof(message_), "%s failed: %s", connectingSsid_.c_str(), why);

    app::AppState& state = app::state();
    {
        app::AppState::Lock lock(state);
        state.status().wifi = app::WifiPhase::Failed;
        state.status().ip[0] = '\0';
        state.touch();
    }
    LOG_W(kTag, "%s; next attempt in %lu ms", message_, (unsigned long)(nextAttemptMs_ - nowMs));

    // Repeated failures across every saved network almost always mean the panel has
    // been moved. Bring the portal up so it can be reconfigured, but keep the saved
    // networks in case it is just a router reboot.
    size_t saved = 0;
    {
        app::AppState::Lock lock(state);
        saved = state.config().wifi.networks.size();
    }
    if (!portalActive_ && saved > 0 && failures_ >= saved * 3) {
        startPortal("cannot reach any saved network");
    }
}

void WifiManager::refreshStatus() {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    app::RuntimeStatus& status = state.status();
    const int8_t rssi = static_cast<int8_t>(WiFi.RSSI());
    if (status.rssi != rssi) {
        status.rssi = rssi;
        state.touch();
    }
}

void WifiManager::loop(uint32_t nowMs) {
    if (g_dnsRunning) {
        g_dns.processNextRequest();
    }

    if (scanRunning_) {
        const int16_t found = WiFi.scanComplete();
        if (found >= 0) {
            scanResults_.clear();
            for (int16_t i = 0; i < found && scanResults_.size() < kMaxScanEntries; ++i) {
                ScanEntry entry;
                entry.ssid = WiFi.SSID(i).c_str();
                if (entry.ssid.empty()) {
                    continue;  // hidden network; joined explicitly, not from the list
                }
                entry.rssi = WiFi.RSSI(i);
                entry.channel = static_cast<uint8_t>(WiFi.channel(i));
                entry.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
                scanResults_.push_back(std::move(entry));
            }
            // Strongest first, and drop duplicate SSIDs from mesh/repeater setups.
            std::stable_sort(scanResults_.begin(), scanResults_.end(),
                             [](const ScanEntry& a, const ScanEntry& b) { return a.rssi > b.rssi; });
            scanResults_.erase(
                std::unique(scanResults_.begin(), scanResults_.end(),
                            [](const ScanEntry& a, const ScanEntry& b) { return a.ssid == b.ssid; }),
                scanResults_.end());
            markKnownNetworks();
            WiFi.scanDelete();
            scanRunning_ = false;
            scanReady_ = true;
            LOG_I(kTag, "scan found %u networks", (unsigned)scanResults_.size());
        } else if (found == WIFI_SCAN_FAILED) {
            scanRunning_ = false;
            scanReady_ = true;
            LOG_W(kTag, "scan failed");
        }
    }

    const wl_status_t status = WiFi.status();

    if (connecting_) {
        if (status == WL_CONNECTED) {
            onConnected();
        } else if (nowMs - attemptStartedMs_ > kConnectTimeoutMs) {
            onFailed(nowMs, status == WL_NO_SSID_AVAIL ? "not found" : "timeout");
        } else if (status == WL_CONNECT_FAILED) {
            onFailed(nowMs, "wrong password or rejected");
        }
        return;
    }

    if (status == WL_CONNECTED) {
        if (nowMs - lastStatusPushMs_ > kStatusPushIntervalMs) {
            lastStatusPushMs_ = nowMs;
            refreshStatus();
        }
        return;
    }

    // Disconnected and not currently attempting: honour the backoff.
    if (nowMs >= nextAttemptMs_) {
        if (!startConnectAttempt(nowMs)) {
            // Nothing saved to try.
            nextAttemptMs_ = nowMs + 5000;
            if (!portalActive_) {
                startPortal("no saved networks");
            }
        }
    }
}

}  // namespace net
