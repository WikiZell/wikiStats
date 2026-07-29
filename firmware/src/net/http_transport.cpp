#include "http_transport.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "../log.h"

namespace net {

namespace {
constexpr const char* kTag = "http";
}

const char* fetchResultName(FetchResult result) {
    switch (result) {
        case FetchResult::Ok:
            return "ok";
        case FetchResult::NotApplicable:
            return "not-applicable";
        case FetchResult::NotConfigured:
            return "not-configured";
        case FetchResult::NetworkError:
            return "network-error";
        case FetchResult::HttpError:
            return "http-error";
        case FetchResult::PayloadError:
            return "payload-error";
    }
    return "unknown";
}

bool HttpTelemetryTransport::begin() {
    started_ = true;
    return true;
}

void HttpTelemetryTransport::end() { started_ = false; }

void HttpTelemetryTransport::loop(uint32_t) {
    // Nothing to do: this transport only acts when asked to fetch.
}

bool HttpTelemetryTransport::healthy() const {
    return started_ && WiFi.status() == WL_CONNECTED;
}

FetchResult HttpTelemetryTransport::fetch(const fp::DeviceConfig& device, fp::Telemetry& out,
                                          std::string& errorOut) {
    errorOut.clear();
    if (device.baseUrl.empty()) {
        errorOut = "no address configured";
        return FetchResult::NotConfigured;
    }
    if (WiFi.status() != WL_CONNECTED) {
        errorOut = "Wi-Fi down";
        return FetchResult::NetworkError;
    }

    const std::string url = device.telemetryUrl();
    WiFiClient client;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(static_cast<int32_t>(timeoutMs_));
    http.setTimeout(static_cast<uint16_t>(timeoutMs_));
    // Follow at most one redirect: an agent behind a reverse proxy is common, an
    // infinite redirect chain on an embedded client is not recoverable.
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setRedirectLimit(1);

    ++requests_;
    if (!http.begin(client, url.c_str())) {
        ++failures_;
        errorOut = "malformed URL";
        return FetchResult::NotConfigured;
    }
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", FP_PRODUCT_NAME "/" FP_FIRMWARE_VERSION);
    if (device.auth == fp::DeviceAuth::Bearer && !device.token.empty()) {
        String header = "Bearer ";
        header += device.token.c_str();
        http.addHeader("Authorization", header);
    }

    const int status = http.GET();
    if (status <= 0) {
        ++failures_;
        errorOut = HTTPClient::errorToString(status).c_str();
        http.end();
        return FetchResult::NetworkError;
    }
    if (status != HTTP_CODE_OK) {
        ++failures_;
        char buffer[48];
        snprintf(buffer, sizeof(buffer), "HTTP %d", status);
        errorOut = buffer;
        http.end();
        return FetchResult::HttpError;
    }

    const int declared = http.getSize();
    if (declared > 0 && static_cast<uint32_t>(declared) > maxPayloadBytes_) {
        // Refuse before reading a byte of the body.
        ++failures_;
        errorOut = "response exceeds payload limit";
        http.end();
        return FetchResult::PayloadError;
    }

    std::string payload;
    payload.reserve(declared > 0 ? static_cast<size_t>(declared) + 1 : 2048);
    WiFiClient* stream = http.getStreamPtr();
    uint8_t chunk[512];
    const uint32_t deadline = millis() + timeoutMs_;
    while (http.connected() && millis() < deadline) {
        const size_t available = stream->available();
        if (available == 0) {
            if (declared > 0 && payload.size() >= static_cast<size_t>(declared)) {
                break;
            }
            if (declared <= 0 && !stream->connected()) {
                break;
            }
            delay(2);
            continue;
        }
        const size_t want = available > sizeof(chunk) ? sizeof(chunk) : available;
        const int read = stream->readBytes(chunk, want);
        if (read <= 0) {
            break;
        }
        if (payload.size() + static_cast<size_t>(read) > maxPayloadBytes_) {
            // Chunked responses have no declared length, so the cap is enforced here.
            ++failures_;
            errorOut = "response exceeds payload limit";
            http.end();
            return FetchResult::PayloadError;
        }
        payload.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(read));
        if (declared > 0 && payload.size() >= static_cast<size_t>(declared)) {
            break;
        }
    }
    http.end();

    if (payload.empty()) {
        ++failures_;
        errorOut = "empty response";
        return FetchResult::PayloadError;
    }

    const fp::ParseStatus parsed = fp::parseTelemetry(payload.c_str(), payload.size(), out,
                                                      maxPayloadBytes_);
    if (parsed != fp::ParseStatus::Ok) {
        ++failures_;
        errorOut = fp::parseStatusText(parsed);
        LOG_D(kTag, "%s: %s (%u bytes)", device.id.c_str(), errorOut.c_str(),
              (unsigned)payload.size());
        return FetchResult::PayloadError;
    }
    if (!device.id.empty() && out.deviceId != device.id) {
        // The agent at this address is a different machine than the one we stored,
        // most likely a recycled DHCP lease. Merging would corrupt the history.
        ++failures_;
        errorOut = "device id mismatch";
        return FetchResult::PayloadError;
    }
    return FetchResult::Ok;
}

}  // namespace net
