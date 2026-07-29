// Host-side test runner for fleet_core.
//
//   cd firmware
//   pio test -e native
//
// Everything under test here is free of Arduino headers on purpose: parsing,
// formatting, thresholds, gestures, carousel timing, device merging and
// configuration migration are exactly the parts that are painful to debug through a
// 320x240 screen, so they are verified on a PC first.

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void suite_units(void);
void suite_telemetry(void);
void suite_thresholds(void);
void suite_gesture(void);
void suite_carousel(void);
void suite_devices(void);
void suite_config(void);
void suite_topics(void);

int main(int, char**) {
    UNITY_BEGIN();
    suite_units();
    suite_telemetry();
    suite_thresholds();
    suite_gesture();
    suite_carousel();
    suite_devices();
    suite_config();
    suite_topics();
    return UNITY_END();
}
