// mDNS: advertise this panel, and find agents publishing _fleetpanel._tcp.
//
// The TXT record contract is defined in docs/discovery.md and produced by the Python
// agent's `build_txt_properties`. Only `id` is required; everything else has a
// sensible fallback so a minimal agent implementation still shows up.
//
// `scan()` blocks for a few seconds inside the network task. It is never called from
// the LVGL task.
#pragma once

#include <string>
#include <vector>

#include "fp_devices.h"

namespace net {

class MdnsDiscovery {
   public:
    // Starts the responder and advertises the panel's own web UI as _http._tcp so
    // "wikistats.local" works from a browser.
    bool begin(const std::string& hostname);
    void end();
    bool running() const { return running_; }

    // Queries _fleetpanel._tcp and returns one DeviceConfig per responder.
    std::vector<fp::DeviceConfig> scan(uint32_t timeoutMs = 3000);

    uint32_t lastScanMs() const { return lastScanMs_; }
    size_t lastFoundCount() const { return lastFound_; }

   private:
    bool running_ = false;
    std::string hostname_;
    uint32_t lastScanMs_ = 0;
    size_t lastFound_ = 0;
};

}  // namespace net
