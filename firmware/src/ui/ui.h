// The LVGL user interface.
//
// Runs entirely on the Arduino loop task (core 1). It reads shared state under the
// AppState lock and never performs network I/O, so a device that times out cannot
// drop a frame.
#pragma once

#include <cstdint>

namespace ui {

bool begin();

// Call from loop(). Reads touch, drives gestures and the carousel, repaints when
// AppState's revision changes, and runs lv_timer_handler.
void tick();

// Screen navigation, also used by the physical recovery path in main().
void showDashboard();
void showSettings();
void showWifiSetup();
void showDeviceList();
void showDiagnostics();

// Full-screen message shown before the UI exists (boot errors, portal details).
void showBootMessage(const char* title, const char* detail);

uint32_t lastInteractionMs();

}  // namespace ui
