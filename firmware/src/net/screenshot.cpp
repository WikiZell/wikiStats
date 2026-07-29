#include "screenshot.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <lvgl.h>

#include "../log.h"

namespace shot {

namespace {

constexpr const char* kTag = "shot";
constexpr uint16_t kWidth = 320;
constexpr uint16_t kHeight = 240;
constexpr uint32_t kWriteTimeoutMs = 5000;

WiFiServer* g_server = nullptr;
uint16_t g_port = 0;
WiFiClient g_client;

volatile bool g_armed = false;
volatile bool g_capturing = false;
int32_t g_expectedY = 0;
uint32_t g_captures = 0;
uint32_t g_startedMs = 0;

void finish(const char* why) {
    if (g_client) {
        g_client.flush();
        g_client.stop();
    }
    if (g_capturing) {
        LOG_I(kTag, "capture %s after %lu ms", why, (unsigned long)(millis() - g_startedMs));
    }
    g_capturing = false;
    g_expectedY = 0;
}

}  // namespace

void begin(uint16_t port) {
    if (g_server != nullptr && g_port == port) {
        return;
    }
    end();
    g_port = port;
    g_server = new WiFiServer(port);
    g_server->begin();
    g_server->setNoDelay(true);
    LOG_I(kTag, "screenshot listener on port %u", port);
}

void end() {
    finish("stopped");
    if (g_server != nullptr) {
        g_server->end();
        delete g_server;
        g_server = nullptr;
    }
    g_port = 0;
}

bool running() { return g_server != nullptr; }
bool capturing() { return g_capturing; }
uint32_t captureCount() { return g_captures; }

void poll() {
    if (g_server == nullptr || !g_server->hasClient()) {
        return;
    }
    if (g_capturing || g_armed) {
        // One capture at a time; a second caller is told so rather than left hanging.
        WiFiClient reject = g_server->accept();
        reject.print("BUSY");
        reject.stop();
        return;
    }
    g_client = g_server->accept();
    g_client.setNoDelay(true);
    g_armed = true;
}

void serviceUi() {
    if (!g_armed || g_capturing) {
        return;
    }
    g_armed = false;
    if (!g_client || !g_client.connected()) {
        return;
    }

    uint8_t header[8] = {'W', 'S', 'S', '1', 0, 0, 0, 0};
    header[4] = static_cast<uint8_t>(kWidth & 0xFF);
    header[5] = static_cast<uint8_t>(kWidth >> 8);
    header[6] = static_cast<uint8_t>(kHeight & 0xFF);
    header[7] = static_cast<uint8_t>(kHeight >> 8);
    if (g_client.write(header, sizeof(header)) != sizeof(header)) {
        finish("header write failed");
        return;
    }

    g_capturing = true;
    g_expectedY = 0;
    g_startedMs = millis();
    // Force a full redraw so the bands arrive top to bottom with nothing missing.
    lv_obj_invalidate(lv_scr_act());
}

void writeBand(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const uint16_t* pixels) {
    if (!g_capturing || pixels == nullptr) {
        return;
    }
    if (!g_client || !g_client.connected()) {
        finish("client disconnected");
        return;
    }
    // Only accept the contiguous full-width bands produced by the invalidate above.
    // Anything else is an ordinary partial redraw that would corrupt the stream.
    if (y1 != g_expectedY || x1 != 0 || x2 != kWidth - 1) {
        return;
    }

    const int32_t rows = y2 - y1 + 1;
    const size_t bytes = static_cast<size_t>(rows) * kWidth * sizeof(uint16_t);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(pixels);

    size_t sent = 0;
    const uint32_t deadline = millis() + kWriteTimeoutMs;
    while (sent < bytes) {
        if (millis() > deadline || !g_client.connected()) {
            finish("write timed out");
            return;
        }
        const size_t written = g_client.write(data + sent, bytes - sent);
        if (written == 0) {
            delay(1);  // socket buffer full; yield rather than spin
            continue;
        }
        sent += written;
    }

    g_expectedY = y2 + 1;
    if (g_expectedY >= kHeight) {
        ++g_captures;
        finish("complete");
    }
}

}  // namespace shot
