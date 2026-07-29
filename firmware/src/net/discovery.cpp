#include "discovery.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include "../log.h"

namespace net {

namespace {

constexpr const char* kTag = "mdns";
constexpr const char* kServiceName = FP_MDNS_SERVICE;  // "fleetpanel"
constexpr const char* kServiceProto = "tcp";
constexpr size_t kMaxResults = 32;

std::string txtOrDefault(int index, const char* key, const char* fallback) {
    const String value = MDNS.txt(index, key);
    if (value.length() == 0) {
        return fallback;
    }
    return std::string(value.c_str());
}

}  // namespace

bool MdnsDiscovery::begin(const std::string& hostname) {
    if (running_ && hostname_ == hostname) {
        return true;
    }
    end();
    hostname_ = hostname;
    if (!MDNS.begin(hostname.c_str())) {
        LOG_W(kTag, "responder failed to start for %s.local", hostname.c_str());
        return false;
    }
    // Advertise the configuration website, not a telemetry service: the panel is a
    // consumer of _fleetpanel._tcp, never a provider of it.
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "product", FP_PRODUCT_NAME);
    MDNS.addServiceTxt("http", "tcp", "version", FP_FIRMWARE_VERSION);
    MDNS.addServiceTxt("http", "tcp", "path", "/");
    running_ = true;
    LOG_I(kTag, "responder up: http://%s.local/", hostname.c_str());
    return true;
}

void MdnsDiscovery::end() {
    if (running_) {
        MDNS.end();
        running_ = false;
    }
}

std::vector<fp::DeviceConfig> MdnsDiscovery::scan(uint32_t timeoutMs) {
    std::vector<fp::DeviceConfig> found;
    if (WiFi.status() != WL_CONNECTED) {
        return found;
    }
    if (!running_ && !begin(hostname_.empty() ? "wikistats" : hostname_)) {
        return found;
    }

    // ESPmDNS on Arduino-ESP32 2.x has no timeout parameter; the query blocks for
    // its own internal window. That is acceptable here because this runs on the
    // network task, never on the LVGL task.
    (void)timeoutMs;
    const int count = MDNS.queryService(kServiceName, kServiceProto);
    lastScanMs_ = millis();
    if (count <= 0) {
        lastFound_ = 0;
        return found;
    }

    for (int i = 0; i < count && found.size() < kMaxResults; ++i) {
        // The stable device ID is the only field the panel cannot invent. Without it
        // two agents could be merged into one entry, or one agent could appear twice
        // after a DHCP change.
        const std::string id = txtOrDefault(i, "id", "");
        if (id.empty()) {
            LOG_D(kTag, "ignoring responder %s with no id TXT record",
                  MDNS.hostname(i).c_str());
            continue;
        }
        const uint16_t schema =
            static_cast<uint16_t>(atoi(txtOrDefault(i, "schema", "1").c_str()));
        if (schema != 1) {
            LOG_D(kTag, "ignoring %s: TXT schema=%u is not supported", id.c_str(), schema);
            continue;
        }

        fp::DeviceConfig device;
        device.id = id;
        device.source = fp::DeviceSource::Mdns;
        device.name = txtOrDefault(i, "name", MDNS.hostname(i).c_str());
        device.path = txtOrDefault(i, "path", "/api/v1/telemetry");
        device.platform = txtOrDefault(i, "platform", "linux");
        device.auth = fp::deviceAuthFromName(txtOrDefault(i, "auth", "none"));

        char url[80];
        snprintf(url, sizeof(url), "http://%s:%u", MDNS.IP(i).toString().c_str(), MDNS.port(i));
        device.baseUrl = url;
        found.push_back(std::move(device));
    }

    lastFound_ = found.size();
    LOG_I(kTag, "discovery found %u agent%s", (unsigned)found.size(),
          found.size() == 1 ? "" : "s");
    return found;
}

}  // namespace net
