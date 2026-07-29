// Shared internals between ui.cpp and ui_settings.cpp.
#pragma once

#include <lvgl.h>

#include "ui.h"
#include "ui_theme.h"

namespace ui {

// Screens are created lazily and kept, so switching pages is a load, not a rebuild.
lv_obj_t* dashboardScreen();
lv_obj_t* settingsScreen();
lv_obj_t* wifiScreen();
lv_obj_t* deviceListScreen();
lv_obj_t* diagnosticsScreen();

void buildSettingsScreen();
void buildWifiScreen();
void buildDeviceListScreen();
void buildDiagnosticsScreen();

void refreshSettingsScreen();
void refreshWifiScreen();
void refreshDeviceListScreen();
void refreshDiagnosticsScreen();

// Marks user activity: pauses the carousel and wakes the backlight.
void noteInteraction();

// Sets the currently displayed dashboard page (index into visibleOrder()).
void setPageIndex(int index);
int pageIndex();

void toast(const char* text);

}  // namespace ui
