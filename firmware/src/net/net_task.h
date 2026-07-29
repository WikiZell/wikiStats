// The network task.
//
// One FreeRTOS task, pinned to core 0 (the core the Wi-Fi driver already runs on),
// owns every operation that can block: DNS, TCP connects, HTTP reads, mDNS queries,
// MQTT, OTA. The LVGL task on core 1 never calls any of them. That split is the
// whole reason the UI stays at a steady frame rate while a device times out.
//
// Communication with the UI goes through AppState's mutex and revision counter, not
// through direct calls, so neither side can stall the other for longer than the
// handful of microseconds a lock is held.
#pragma once

#include <cstdint>

#include "discovery.h"
#include "http_transport.h"
#include "mqtt_transport.h"

namespace net {

void startNetworkTask();

// Ask for an immediate mDNS sweep (the "Scan" button and POST /api/discovery/scan).
void requestDiscoveryScan();

// Re-read MQTT settings from the configuration and reconnect if they changed.
void requestTransportReload();

HttpTelemetryTransport& httpTransport();
MqttTelemetryTransport& mqttTransport();
MdnsDiscovery& mdns();

// Which transport is currently supplying data, for the diagnostics page.
const char* activeTransportName();

}  // namespace net
