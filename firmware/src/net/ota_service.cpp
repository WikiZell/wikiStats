#include "ota_service.h"

#include <ArduinoOTA.h>

#include "../log.h"

namespace net {

namespace {
constexpr const char* kTag = "ota";
OtaService* g_service = nullptr;
}  // namespace

OtaService& ota() {
    static OtaService instance;
    return instance;
}

void OtaService::begin(const std::string& hostname, const std::string& password) {
    if (enabled_) {
        return;
    }
    if (password.empty()) {
        // An unauthenticated OTA listener on a LAN is a remote code execution
        // primitive. Refusing is the correct default, and the web uploader still
        // works because it is behind the admin session.
        LOG_W(kTag, "ArduinoOTA disabled: no password set (web upload still available)");
        return;
    }
    g_service = this;

    ArduinoOTA.setHostname(hostname.c_str());
    ArduinoOTA.setPassword(password.c_str());
    ArduinoOTA.setRebootOnSuccess(true);

    ArduinoOTA.onStart([]() {
        const char* what = ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem";
        if (g_service != nullptr) {
            g_service->inProgress_ = true;
            g_service->progress_ = 0;
            g_service->lastError_[0] = '\0';
        }
        LOG_W(kTag, "update starting (%s)", what);
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        if (g_service == nullptr || total == 0) {
            return;
        }
        const uint8_t percent = static_cast<uint8_t>((done * 100ULL) / total);
        if (percent != g_service->progress_) {
            g_service->progress_ = percent;
            if (percent % 10 == 0) {
                LOG_I(kTag, "update %u%%", percent);
            }
        }
    });
    ArduinoOTA.onEnd([]() {
        if (g_service != nullptr) {
            g_service->inProgress_ = false;
            g_service->progress_ = 100;
        }
        LOG_W(kTag, "update complete, rebooting");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        const char* text = "unknown";
        switch (error) {
            case OTA_AUTH_ERROR:
                text = "authentication failed";
                break;
            case OTA_BEGIN_ERROR:
                text = "begin failed";
                break;
            case OTA_CONNECT_ERROR:
                text = "connect failed";
                break;
            case OTA_RECEIVE_ERROR:
                text = "receive failed";
                break;
            case OTA_END_ERROR:
                text = "end failed";
                break;
        }
        if (g_service != nullptr) {
            g_service->inProgress_ = false;
            snprintf(g_service->lastError_, sizeof(g_service->lastError_), "%s", text);
        }
        LOG_E(kTag, "update failed: %s", text);
    });

    ArduinoOTA.begin();
    enabled_ = true;
    LOG_I(kTag, "ArduinoOTA listening as %s.local", hostname.c_str());
}

void OtaService::end() {
    if (!enabled_) {
        return;
    }
    ArduinoOTA.end();
    enabled_ = false;
}

void OtaService::loop() {
    if (enabled_) {
        ArduinoOTA.handle();
    }
}

}  // namespace net
