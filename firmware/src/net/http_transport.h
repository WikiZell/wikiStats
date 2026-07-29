#pragma once

#include "transport.h"

namespace net {

// Blocking HTTP client, but only ever blocking the network task.
//
// Protections that matter on a device with ~200 KiB of free heap:
//   * connect and read timeouts from configuration (default 4 s)
//   * Content-Length checked against the payload ceiling *before* reading
//   * chunked/unknown-length responses read with a hard byte cap
//   * one reusable receive buffer rather than a String that grows by realloc
class HttpTelemetryTransport final : public TelemetryTransport {
   public:
    const char* name() const override { return "http"; }
    bool begin() override;
    void end() override;
    void loop(uint32_t nowMs) override;
    bool healthy() const override;
    bool isPush() const override { return false; }

    FetchResult fetch(const fp::DeviceConfig& device, fp::Telemetry& out,
                      std::string& errorOut) override;

    void setTimeoutMs(uint32_t timeoutMs) { timeoutMs_ = timeoutMs; }
    void setMaxPayloadBytes(uint32_t maxBytes) { maxPayloadBytes_ = maxBytes; }

    uint32_t requests() const { return requests_; }
    uint32_t failures() const { return failures_; }

   private:
    uint32_t timeoutMs_ = 4000;
    uint32_t maxPayloadBytes_ = 16384;
    uint32_t requests_ = 0;
    uint32_t failures_ = 0;
    bool started_ = false;
};

}  // namespace net
