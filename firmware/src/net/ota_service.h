// Over-the-air firmware update.
//
// Two independent paths, both writing the inactive OTA slot so a failed update
// cannot brick a wall-mounted panel:
//
//   * ArduinoOTA (espota) - what `pio run -e cyd-ota -t upload` uses during
//     development. Password protected: without one it would let anyone on the LAN
//     replace the firmware.
//   * POST /api/firmware - the browser uploader in the web UI, authenticated by the
//     same session and CSRF checks as every other mutating endpoint.
//
// Both refuse to start while the panel is applying a configuration write, so a
// reboot can never land in the middle of a flash rewrite.
#pragma once

#include <Arduino.h>

#include <string>

namespace net {

class OtaService {
   public:
    // `password` empty disables ArduinoOTA rather than leaving it open.
    void begin(const std::string& hostname, const std::string& password);
    void end();
    void loop();

    bool enabled() const { return enabled_; }
    bool inProgress() const { return inProgress_; }
    uint8_t progressPercent() const { return progress_; }
    const char* lastError() const { return lastError_; }

   private:
    bool enabled_ = false;
    bool inProgress_ = false;
    uint8_t progress_ = 0;
    char lastError_[64] = {0};
};

OtaService& ota();

}  // namespace net
