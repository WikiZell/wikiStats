// Frame capture over TCP, for looking at the panel when you cannot look at the panel.
//
// A full 320x240 RGB565 frame is 153 600 bytes. The board has around 100 KiB of
// free heap, so buffering a frame is not an option and neither is an
// AsyncWebServer pull-response, which needs random access to the data.
//
// Instead the frame is streamed straight out of the LVGL flush callback as it is
// drawn: a client connects, the UI task invalidates the screen, and each ~40-line
// band goes to the socket the moment it has been pushed to the panel. Zero extra
// heap, and the only cost is that the UI stalls for the length of the capture -
// acceptable for something that runs on demand.
//
// Wire format, little-endian:
//
//   magic   4 bytes  "WSS1"
//   width   uint16
//   height  uint16
//   pixels  width * height * uint16, RGB565, top row first
//
// `tools/screenshot.py` turns that into a PNG.
#pragma once

#include <cstdint>

namespace shot {

// Starts the capture listener. Safe to call repeatedly with the same port.
void begin(uint16_t port);
void end();
bool running();

// Network task: accepts a waiting client and arms a capture.
void poll();

// UI task, once per tick: starts a capture if one is armed.
void serviceUi();

// Called from the LVGL flush callback for every band while a capture is active.
void writeBand(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const uint16_t* pixels);

bool capturing();
uint32_t captureCount();

}  // namespace shot
