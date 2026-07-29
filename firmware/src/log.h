// Diagnostics: serial, an in-memory ring for the web UI, and a Wi-Fi log console.
//
// The Wi-Fi console is the reason this exists as a module rather than a handful of
// Serial.printf calls. Once the panel is screwed to a wall the USB port is gone, so
// every log line also goes to a TCP listener (default port 23) and to a bounded ring
// buffer that `GET /api/logs` returns. Both are read-only; nothing accepts commands.
//
// Formatting happens into a fixed stack buffer. There is no Arduino String anywhere
// in here: unbounded concatenation in a logger is how an ESP32 fragments its heap.
#pragma once

#include <Arduino.h>

#include <cstdarg>
#include <cstddef>

namespace fplog {

enum class Level : uint8_t {
    Error = 0,
    Warn,
    Info,
    Debug,
    Trace,
};

Level levelFromName(const char* name);
const char* levelName(Level level);

void begin(uint32_t baud);
void setLevel(Level level);
Level level();

// Wi-Fi console. Safe to call repeatedly; a second call with the same port is a
// no-op. Must be driven by `pump()` from a task, never from an ISR.
void startConsole(uint16_t port);
void stopConsole();
bool consoleRunning();
uint8_t consoleClients();
void pump();

void write(Level level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void vwrite(Level level, const char* tag, const char* fmt, va_list args);

// Copies the ring buffer, oldest first, into `out`. Returns bytes written.
size_t snapshot(char* out, size_t capacity);
size_t droppedLines();

}  // namespace fplog

#define LOG_E(tag, ...) ::fplog::write(::fplog::Level::Error, tag, __VA_ARGS__)
#define LOG_W(tag, ...) ::fplog::write(::fplog::Level::Warn, tag, __VA_ARGS__)
#define LOG_I(tag, ...) ::fplog::write(::fplog::Level::Info, tag, __VA_ARGS__)
#define LOG_D(tag, ...) ::fplog::write(::fplog::Level::Debug, tag, __VA_ARGS__)
#define LOG_T(tag, ...) ::fplog::write(::fplog::Level::Trace, tag, __VA_ARGS__)
