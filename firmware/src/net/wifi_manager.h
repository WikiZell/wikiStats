// Wi-Fi lifecycle: multiple saved networks, priority order, backoff, and a captive
// setup portal.
//
// Recovery matters more than elegance here. The panel has no keyboard, so:
//   * Several networks can be saved, tried in priority order.
//   * A failed connection retries with exponential backoff instead of hammering the
//     access point, and rotates to the next saved network after repeated failures.
//   * Holding the touchscreen during boot starts the portal *without* erasing the
//     saved networks, so a user who moved house can add a network without losing
//     their device list.
//   * With no saved network at all, the portal comes up automatically.
//
// Scanning is asynchronous: `WiFi.scanNetworks(true)` returns immediately and the
// result is collected on a later loop, so the UI never freezes mid-scan.
#pragma once

#include <Arduino.h>

#include <string>
#include <vector>

namespace net {

struct ScanEntry {
    std::string ssid;
    int32_t rssi = 0;
    uint8_t channel = 0;
    bool secure = true;
    bool known = false;  // already in the saved list
};

class WifiManager {
   public:
    void begin();
    void loop(uint32_t nowMs);

    // Physical recovery: holding the touchscreen during boot brings the setup
    // portal up on the next begin() *without* erasing saved networks or devices.
    void forcePortalOnBoot() { forcePortal_ = true; }

    // Portal control. `preserveSettings` is always true - the portal never wipes
    // anything; forgetting networks is a separate, explicit action.
    void startPortal(const char* reason);
    void stopPortal();
    bool portalActive() const { return portalActive_; }

    void requestScan();
    bool scanInProgress() const { return scanRunning_; }
    // True once after a scan completes, so the UI can refresh exactly once.
    bool takeScanResult();
    const std::vector<ScanEntry>& scanResults() const { return scanResults_; }

    // Saves the network (unless `remember` is false) and immediately tries it.
    bool connectTo(const std::string& ssid, const std::string& password, bool remember,
                   bool hidden = false);
    void reconnectNow();
    void forget(const std::string& ssid);
    void forgetAll();

    bool connected() const;
    const char* lastMessage() const { return message_; }
    uint32_t failureCount() const { return failures_; }

   private:
    void applyHostname();
    bool startConnectAttempt(uint32_t nowMs);
    void onConnected();
    void onFailed(uint32_t nowMs, const char* why);
    void refreshStatus();
    void markKnownNetworks();

    bool portalActive_ = false;
    bool forcePortal_ = false;
    bool scanRunning_ = false;
    bool scanReady_ = false;
    bool connecting_ = false;
    size_t nextNetworkIndex_ = 0;
    uint32_t attemptStartedMs_ = 0;
    uint32_t nextAttemptMs_ = 0;
    uint32_t backoffMs_ = 2000;
    uint32_t failures_ = 0;
    uint32_t lastStatusPushMs_ = 0;
    char message_[96] = {0};
    std::string connectingSsid_;
    std::vector<ScanEntry> scanResults_;
};

WifiManager& wifi();

}  // namespace net
