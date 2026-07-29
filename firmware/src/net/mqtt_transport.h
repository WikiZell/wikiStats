#pragma once

#include <functional>
#include <string>

#include "fp_mqtt_topics.h"
#include "transport.h"

namespace net {

// MQTT subscriber built on PubSubClient.
//
// PubSubClient blocks during connect, which is exactly why every call into this
// class happens from the network task and never from the LVGL task. Reconnection
// uses exponential backoff with a cap so a broker that is down does not turn into a
// connect storm.
//
// Retained `meta` messages double as a discovery source: subscribing to
// `<base>/devices/+/meta` tells the panel about every agent in the fleet within one
// broker round-trip, without waiting a full telemetry interval.
class MqttTelemetryTransport final : public TelemetryTransport {
   public:
    // (deviceId, telemetry) - a fully parsed sample.
    using SampleHandler = std::function<void(const std::string&, const fp::Telemetry&)>;
    // (deviceId, name, baseUrl, platform) from a retained meta document.
    using MetaHandler =
        std::function<void(const std::string&, const std::string&, const std::string&,
                           const std::string&)>;
    // (deviceId, online)
    using AvailabilityHandler = std::function<void(const std::string&, bool)>;

    const char* name() const override { return "mqtt"; }
    bool begin() override;
    void end() override;
    void loop(uint32_t nowMs) override;
    bool healthy() const override;
    bool isPush() const override { return true; }

    void configure(const std::string& host, uint16_t port, const std::string& username,
                   const std::string& password, const std::string& baseTopic,
                   const std::string& clientId, bool tls);

    void onSample(SampleHandler handler) { sampleHandler_ = std::move(handler); }
    void onMeta(MetaHandler handler) { metaHandler_ = std::move(handler); }
    void onAvailability(AvailabilityHandler handler) { availabilityHandler_ = std::move(handler); }

    void setMaxPayloadBytes(uint32_t maxBytes) { maxPayloadBytes_ = maxBytes; }

    bool connected() const { return connected_; }
    uint32_t messages() const { return messages_; }
    uint32_t lastSampleMs() const { return lastSampleMs_; }
    const std::string& lastError() const { return lastError_; }

    // Exposed for the message callback, which PubSubClient delivers as a free function.
    void handleMessage(const char* topic, const uint8_t* payload, unsigned int length);

   private:
    bool connect(uint32_t nowMs);

    std::string host_;
    uint16_t port_ = 1883;
    std::string username_;
    std::string password_;
    std::string baseTopic_ = "fleetpanel/v1";
    std::string clientId_;
    bool tls_ = false;

    bool enabled_ = false;
    bool connected_ = false;
    uint32_t nextAttemptMs_ = 0;
    uint32_t backoffMs_ = 1000;
    uint32_t messages_ = 0;
    uint32_t lastSampleMs_ = 0;
    uint32_t maxPayloadBytes_ = 16384;
    std::string lastError_;

    SampleHandler sampleHandler_;
    MetaHandler metaHandler_;
    AvailabilityHandler availabilityHandler_;
};

}  // namespace net
