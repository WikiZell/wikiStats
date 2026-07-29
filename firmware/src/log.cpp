#include "log.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>

#include <cstdio>
#include <cstring>

namespace fplog {

namespace {

constexpr size_t kRingBytes = 6144;
constexpr size_t kLineMax = 240;
constexpr uint8_t kMaxConsoleClients = 2;

char g_ring[kRingBytes];
size_t g_ringHead = 0;
bool g_ringWrapped = false;
size_t g_dropped = 0;

Level g_level = Level::Info;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

WiFiServer* g_server = nullptr;
uint16_t g_port = 0;
WiFiClient g_clients[kMaxConsoleClients];

void ringPush(const char* text, size_t length) {
    // Called from several tasks; the critical section is a few hundred cycles.
    portENTER_CRITICAL(&g_mux);
    for (size_t i = 0; i < length; ++i) {
        g_ring[g_ringHead] = text[i];
        g_ringHead = (g_ringHead + 1) % kRingBytes;
        if (g_ringHead == 0) {
            g_ringWrapped = true;
        }
    }
    portEXIT_CRITICAL(&g_mux);
}

}  // namespace

Level levelFromName(const char* name) {
    if (name == nullptr) {
        return Level::Info;
    }
    if (strcmp(name, "error") == 0) return Level::Error;
    if (strcmp(name, "warn") == 0) return Level::Warn;
    if (strcmp(name, "debug") == 0) return Level::Debug;
    if (strcmp(name, "trace") == 0) return Level::Trace;
    return Level::Info;
}

const char* levelName(Level level) {
    switch (level) {
        case Level::Error:
            return "error";
        case Level::Warn:
            return "warn";
        case Level::Debug:
            return "debug";
        case Level::Trace:
            return "trace";
        case Level::Info:
        default:
            return "info";
    }
}

void begin(uint32_t baud) {
    Serial.begin(baud);
    // A short, bounded wait: enough for a host terminal to attach after reset,
    // never enough to delay boot if nothing is listening.
    const uint32_t deadline = millis() + 300;
    while (!Serial && millis() < deadline) {
        delay(10);
    }
    Serial.println();
}

void setLevel(Level level) { g_level = level; }
Level level() { return g_level; }

void startConsole(uint16_t port) {
    if (g_server != nullptr && g_port == port) {
        return;
    }
    stopConsole();
    g_port = port;
    g_server = new WiFiServer(port);
    g_server->begin();
    g_server->setNoDelay(true);
    LOG_I("log", "Wi-Fi log console listening on port %u", port);
}

void stopConsole() {
    for (WiFiClient& client : g_clients) {
        if (client) {
            client.stop();
        }
    }
    if (g_server != nullptr) {
        g_server->end();
        delete g_server;
        g_server = nullptr;
    }
    g_port = 0;
}

bool consoleRunning() { return g_server != nullptr; }

uint8_t consoleClients() {
    uint8_t count = 0;
    for (WiFiClient& client : g_clients) {
        if (client && client.connected()) {
            ++count;
        }
    }
    return count;
}

void pump() {
    if (g_server == nullptr) {
        return;
    }
    if (g_server->hasClient()) {
        bool placed = false;
        for (WiFiClient& client : g_clients) {
            if (!client || !client.connected()) {
                if (client) {
                    client.stop();
                }
                client = g_server->accept();
                client.setNoDelay(true);
                client.printf("%s %s log console. Read-only.\r\n", FP_PRODUCT_NAME,
                              FP_FIRMWARE_VERSION);
                placed = true;
                break;
            }
        }
        if (!placed) {
            // Refuse politely rather than silently dropping the connection.
            WiFiClient reject = g_server->accept();
            reject.println("log console busy");
            reject.stop();
        }
    }
    for (WiFiClient& client : g_clients) {
        // The console is output-only; anything typed is discarded so a stray
        // keystroke cannot be mistaken for a command channel.
        while (client && client.connected() && client.available()) {
            client.read();
        }
    }
}

void vwrite(Level level, const char* tag, const char* fmt, va_list args) {
    if (static_cast<uint8_t>(level) > static_cast<uint8_t>(g_level)) {
        return;
    }
    char line[kLineMax];
    const uint32_t now = millis();
    int offset = snprintf(line, sizeof(line), "[%8lu][%c][%s] ", (unsigned long)now,
                          levelName(level)[0], tag != nullptr ? tag : "-");
    if (offset < 0) {
        return;
    }
    if (static_cast<size_t>(offset) >= sizeof(line)) {
        offset = static_cast<int>(sizeof(line)) - 1;
    }
    const int written = vsnprintf(line + offset, sizeof(line) - static_cast<size_t>(offset), fmt, args);
    size_t length = static_cast<size_t>(offset) + (written > 0 ? static_cast<size_t>(written) : 0);
    if (length >= sizeof(line) - 2) {
        length = sizeof(line) - 3;
        ++g_dropped;  // the message was truncated, which is worth surfacing
    }
    line[length++] = '\r';
    line[length++] = '\n';
    line[length] = '\0';

    Serial.write(reinterpret_cast<const uint8_t*>(line), length);
    ringPush(line, length);

    for (WiFiClient& client : g_clients) {
        if (client && client.connected()) {
            client.write(reinterpret_cast<const uint8_t*>(line), length);
        }
    }
}

void write(Level level, const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vwrite(level, tag, fmt, args);
    va_end(args);
}

size_t snapshot(char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    portENTER_CRITICAL(&g_mux);
    const size_t head = g_ringHead;
    const bool wrapped = g_ringWrapped;
    portEXIT_CRITICAL(&g_mux);

    size_t written = 0;
    if (wrapped) {
        for (size_t i = head; i < kRingBytes && written + 1 < capacity; ++i) {
            out[written++] = g_ring[i];
        }
    }
    for (size_t i = 0; i < head && written + 1 < capacity; ++i) {
        out[written++] = g_ring[i];
    }
    out[written] = '\0';
    return written;
}

size_t droppedLines() { return g_dropped; }

}  // namespace fplog
