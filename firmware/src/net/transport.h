// Telemetry transport abstraction.
//
// Above this interface nothing knows whether a sample arrived over HTTP or MQTT.
// `TransportManager` owns both and applies the mode policy:
//
//   http  - poll every enabled device on a timer
//   mqtt  - subscribe and wait; never poll
//   auto  - prefer MQTT while it is connected *and* delivering recent samples,
//           otherwise fall back to polling. "Connected" alone is not enough: a
//           broker with no publisher looks healthy and would leave the panel blank.
//
// Both implementations run inside the network task. Neither is ever called from the
// LVGL task, so a four-second HTTP timeout cannot stall a redraw.
#pragma once

#include <cstdint>
#include <string>

#include "fp_devices.h"
#include "fp_telemetry.h"

namespace net {

enum class FetchResult : uint8_t {
    Ok = 0,
    NotApplicable,   // this transport is push-based
    NotConfigured,
    NetworkError,
    HttpError,
    PayloadError,
};

const char* fetchResultName(FetchResult result);

class TelemetryTransport {
   public:
    virtual ~TelemetryTransport() = default;

    virtual const char* name() const = 0;
    virtual bool begin() = 0;
    virtual void end() = 0;

    // Called from the network task loop. Push transports do their work here.
    virtual void loop(uint32_t nowMs) = 0;

    // True when this transport could deliver a sample right now.
    virtual bool healthy() const = 0;

    // Pull transports implement this; push transports return NotApplicable.
    virtual FetchResult fetch(const fp::DeviceConfig& device, fp::Telemetry& out,
                              std::string& errorOut) {
        (void)device;
        (void)out;
        errorOut = "transport is push-based";
        return FetchResult::NotApplicable;
    }

    virtual bool isPush() const = 0;
};

}  // namespace net
