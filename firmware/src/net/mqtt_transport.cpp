#include "mqtt_transport.h"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "../log.h"

namespace net {

namespace {

constexpr const char* kTag = "mqtt";
constexpr uint32_t kMaxBackoffMs = 60000;
// Telemetry documents run 1.5-3 KiB. PubSubClient's 256-byte default would silently
// drop every one of them, which is a classic and very confusing failure.
constexpr uint16_t kMqttBufferBytes = 8192;

WiFiClient g_plainClient;
WiFiClientSecure g_tlsClient;
PubSubClient g_client;
MqttTelemetryTransport* g_owner = nullptr;

void trampoline(char* topic, uint8_t* payload, unsigned int length) {
    if (g_owner != nullptr) {
        g_owner->handleMessage(topic, payload, length);
    }
}

}  // namespace

void MqttTelemetryTransport::configure(const std::string& host, uint16_t port,
                                       const std::string& username, const std::string& password,
                                       const std::string& baseTopic, const std::string& clientId,
                                       bool tls) {
    const bool changed = host != host_ || port != port_ || username != username_ ||
                         password != password_ || baseTopic != baseTopic_ || tls != tls_;
    host_ = host;
    port_ = port;
    username_ = username;
    password_ = password;
    baseTopic_ = fp::normaliseBaseTopic(baseTopic);
    clientId_ = clientId;
    tls_ = tls;
    if (changed && connected_) {
        // Reconnect against the new broker rather than leaving a stale session.
        g_client.disconnect();
        connected_ = false;
        nextAttemptMs_ = 0;
        backoffMs_ = 1000;
    }
}

bool MqttTelemetryTransport::begin() {
    if (host_.empty()) {
        lastError_ = "no broker configured";
        return false;
    }
    g_owner = this;
    if (tls_) {
        // No CA pinning: the panel has no clock at boot and no trust store. TLS here
        // buys transport confidentiality on the LAN, not server authentication, and
        // the documentation says so rather than implying more.
        g_tlsClient.setInsecure();
        g_client.setClient(g_tlsClient);
    } else {
        g_client.setClient(g_plainClient);
    }
    g_client.setServer(host_.c_str(), port_);
    g_client.setCallback(trampoline);
    g_client.setBufferSize(kMqttBufferBytes);
    g_client.setKeepAlive(30);
    g_client.setSocketTimeout(5);
    enabled_ = true;
    nextAttemptMs_ = 0;
    backoffMs_ = 1000;
    LOG_I(kTag, "broker %s:%u tls=%d user=%s password=%s", host_.c_str(), port_, tls_ ? 1 : 0,
          username_.empty() ? "<unset>" : username_.c_str(),
          password_.empty() ? "<unset>" : "<set>");
    return true;
}

void MqttTelemetryTransport::end() {
    enabled_ = false;
    connected_ = false;
    if (g_client.connected()) {
        g_client.disconnect();
    }
    g_owner = nullptr;
}

bool MqttTelemetryTransport::healthy() const { return connected_; }

bool MqttTelemetryTransport::connect(uint32_t nowMs) {
    if (WiFi.status() != WL_CONNECTED || host_.empty()) {
        return false;
    }
    if (nowMs < nextAttemptMs_) {
        return false;
    }

    std::string clientId = clientId_;
    if (clientId.empty()) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "wikistats-%08x",
                 static_cast<unsigned>(ESP.getEfuseMac() >> 24));
        clientId = buffer;
    }

    const bool ok = username_.empty()
                        ? g_client.connect(clientId.c_str())
                        : g_client.connect(clientId.c_str(), username_.c_str(),
                                           password_.empty() ? nullptr : password_.c_str());
    if (!ok) {
        connected_ = false;
        char buffer[48];
        snprintf(buffer, sizeof(buffer), "connect failed, state %d", g_client.state());
        lastError_ = buffer;
        // Exponential backoff, capped. A broker that is down must not become a
        // connect storm on the LAN.
        nextAttemptMs_ = nowMs + backoffMs_;
        backoffMs_ = backoffMs_ >= kMaxBackoffMs ? kMaxBackoffMs : backoffMs_ * 2;
        LOG_W(kTag, "%s; retrying in %lu ms", lastError_.c_str(), (unsigned long)backoffMs_);
        return false;
    }

    connected_ = true;
    lastError_.clear();
    backoffMs_ = 1000;

    // Retained meta first: it is the discovery half of MQTT.
    g_client.subscribe(fp::subscriptionFor(baseTopic_, "meta").c_str(), 1);
    g_client.subscribe(fp::subscriptionFor(baseTopic_, "availability").c_str(), 1);
    g_client.subscribe(fp::subscriptionFor(baseTopic_, "telemetry").c_str(), 0);
    LOG_I(kTag, "connected, subscribed under %s/devices/+", baseTopic_.c_str());
    return true;
}

void MqttTelemetryTransport::loop(uint32_t nowMs) {
    if (!enabled_) {
        return;
    }
    if (!g_client.connected()) {
        if (connected_) {
            LOG_W(kTag, "disconnected (state %d)", g_client.state());
            connected_ = false;
        }
        connect(nowMs);
        return;
    }
    g_client.loop();
}

void MqttTelemetryTransport::handleMessage(const char* topic, const uint8_t* payload,
                                           unsigned int length) {
    ++messages_;
    std::string deviceId;
    fp::TopicLeaf leaf = fp::TopicLeaf::Unknown;
    if (!fp::parseDeviceTopic(baseTopic_, topic == nullptr ? "" : topic, deviceId, leaf)) {
        return;  // not ours; another application shares the broker
    }
    if (length == 0 || length > maxPayloadBytes_) {
        LOG_D(kTag, "dropping %u byte message on %s", length, topic);
        return;
    }

    switch (leaf) {
        case fp::TopicLeaf::Availability: {
            const bool online = length >= 6 && memcmp(payload, "online", 6) == 0;
            if (availabilityHandler_) {
                availabilityHandler_(deviceId, online);
            }
            break;
        }
        case fp::TopicLeaf::Meta: {
            if (!metaHandler_) {
                break;
            }
            JsonDocument doc;
            if (deserializeJson(doc, payload, length)) {
                return;
            }
            const char* name = doc["name"] | "";
            const char* platform = doc["platform"] | "linux";
            // Build a base URL from the advertised address so an MQTT-discovered
            // device can also be polled over HTTP if the broker goes away.
            std::string baseUrl;
            JsonArrayConst addresses = doc["http"]["addresses"];
            const int port = doc["http"]["port"] | 8770;
            if (!addresses.isNull()) {
                for (JsonVariantConst item : addresses) {
                    if (item.is<const char*>() && item.as<const char*>() != nullptr) {
                        char buffer[64];
                        snprintf(buffer, sizeof(buffer), "http://%s:%d", item.as<const char*>(),
                                 port);
                        baseUrl = buffer;
                        break;
                    }
                }
            }
            metaHandler_(deviceId, name, baseUrl, platform);
            break;
        }
        case fp::TopicLeaf::Telemetry: {
            if (!sampleHandler_) {
                break;
            }
            fp::Telemetry telemetry;
            const fp::ParseStatus status = fp::parseTelemetry(
                reinterpret_cast<const char*>(payload), length, telemetry, maxPayloadBytes_);
            if (status != fp::ParseStatus::Ok) {
                LOG_D(kTag, "%s: %s", deviceId.c_str(), fp::parseStatusText(status));
                return;
            }
            if (telemetry.deviceId != deviceId) {
                // Topic and payload disagree: either two agents share an ID or
                // something else is publishing here. Dropping is the safe answer.
                LOG_W(kTag, "topic/payload id mismatch: %s vs %s", deviceId.c_str(),
                      telemetry.deviceId.c_str());
                return;
            }
            lastSampleMs_ = millis();
            sampleHandler_(deviceId, telemetry);
            break;
        }
        case fp::TopicLeaf::Unknown:
        default:
            break;
    }
}

}  // namespace net
