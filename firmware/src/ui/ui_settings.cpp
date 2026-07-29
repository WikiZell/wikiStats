// Settings, Wi-Fi provisioning, device management and diagnostics screens.
//
// Everything reachable from the gear icon. These screens mutate the same
// `PanelConfig` the web interface edits, through the same lock and the same
// `requestSave()` path, which is what keeps the two interfaces in step.
//
// The Wi-Fi screen is the one that has to work when nothing else does: no network,
// no browser, no serial console. It therefore does its own scanning, its own
// on-screen keyboard and its own progress reporting, and never shows a saved
// password back to the user.

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include <cstring>
#include <string>

#include "../app_state.h"
#include "../log.h"
#include "../net/net_task.h"
#include "../net/web_server.h"
#include "../net/wifi_manager.h"
#include "fp_units.h"
#include "ui_internal.h"

namespace ui {

namespace {

constexpr const char* kTag = "ui";

lv_obj_t* g_settingsScreen = nullptr;
lv_obj_t* g_wifiScreen = nullptr;
lv_obj_t* g_devicesScreen = nullptr;
lv_obj_t* g_diagScreen = nullptr;

// settings widgets
lv_obj_t* g_carouselSwitch = nullptr;
lv_obj_t* g_carouselSlider = nullptr;
lv_obj_t* g_carouselValue = nullptr;
lv_obj_t* g_brightnessSlider = nullptr;
lv_obj_t* g_brightnessValue = nullptr;
lv_obj_t* g_transportLabel = nullptr;

// wifi widgets
lv_obj_t* g_wifiList = nullptr;
lv_obj_t* g_wifiStatus = nullptr;
lv_obj_t* g_passwordArea = nullptr;
lv_obj_t* g_keyboard = nullptr;
lv_obj_t* g_revealButton = nullptr;
lv_obj_t* g_connectPanel = nullptr;
lv_obj_t* g_connectTitle = nullptr;
std::string g_pendingSsid;
bool g_pendingHidden = false;
uint32_t g_lastWifiListMs = 0;
// True while the password prompt is up. The periodic refresh must not rebuild the
// network list underneath someone who is typing.
bool g_promptOpen = false;

// device widgets
lv_obj_t* g_deviceList = nullptr;

// diagnostics widgets
lv_obj_t* g_diagText = nullptr;

// List rows carry a strdup'd id/SSID in their user data. lv_obj_clean() deletes the
// rows but knows nothing about that pointer, so it has to be freed first or every
// Wi-Fi scan leaks a few hundred bytes on a device that runs for months.
void releaseListUserData(lv_obj_t* list) {
    if (list == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(list); ++i) {
        lv_obj_t* child = lv_obj_get_child(list, static_cast<int32_t>(i));
        void* data = lv_obj_get_user_data(child);
        if (data != nullptr) {
            free(data);
            lv_obj_set_user_data(child, nullptr);
        }
    }
}

lv_obj_t* makeScreen() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, kColorBackground, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    return screen;
}

lv_obj_t* makeTitleBar(lv_obj_t* screen, const char* title, lv_event_cb_t backCb) {
    lv_obj_t* bar = lv_obj_create(screen);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, kScreenW, kHeaderH + 6);
    lv_obj_set_style_bg_color(bar, kColorCard, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_btn_create(bar);
    lv_obj_set_pos(back, 2, 1);
    lv_obj_set_size(back, 46, 30);
    lv_obj_set_style_bg_color(back, kColorCardAlt, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, kColorAccent, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
    lv_obj_t* backLabel = lv_label_create(back);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLabel, kColorText, LV_PART_MAIN);
    lv_obj_center(backLabel);
    lv_obj_add_event_cb(back, backCb, LV_EVENT_CLICKED, nullptr);

    makeLabel(bar, title, &lv_font_montserrat_16, kColorText, 56, 7);
    return bar;
}

void onBackToDashboard(lv_event_t*) {
    noteInteraction();
    showDashboard();
}

void onBackToSettings(lv_event_t*) {
    noteInteraction();
    showSettings();
}

// =========================================================== settings

void onCarouselToggled(lv_event_t* event) {
    noteInteraction();
    const bool enabled = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
    app::AppState& state = app::state();
    {
        app::AppState::Lock lock(state);
        state.config().carousel.enabled = enabled;
        state.touch();
        state.requestSave();
    }
    toast(enabled ? "Carousel on" : "Carousel off");
}

void onCarouselInterval(lv_event_t* event) {
    noteInteraction();
    const int32_t seconds = lv_slider_get_value(lv_event_get_target(event));
    app::AppState& state = app::state();
    {
        app::AppState::Lock lock(state);
        state.config().carousel.intervalSeconds = static_cast<uint32_t>(seconds);
        state.config().carousel.sanitise();
        state.touch();
        state.requestSave();
    }
    lv_label_set_text_fmt(g_carouselValue, "%lds", (long)seconds);
}

void onBrightness(lv_event_t* event) {
    noteInteraction();
    const int32_t percent = lv_slider_get_value(lv_event_get_target(event));
    app::AppState& state = app::state();
    {
        app::AppState::Lock lock(state);
        state.config().display.brightness = static_cast<uint8_t>(percent);
        state.touch();
        state.requestSave();
    }
    lv_label_set_text_fmt(g_brightnessValue, "%ld%%", (long)percent);
}

void onTransportPressed(lv_event_t*) {
    noteInteraction();
    app::AppState& state = app::state();
    fp::TransportMode mode = fp::TransportMode::Auto;
    {
        app::AppState::Lock lock(state);
        // Cycle auto -> http -> mqtt -> auto. Three states are quicker to reach by
        // tapping than a dropdown is on a resistive panel.
        switch (state.config().transport.mode) {
            case fp::TransportMode::Auto:
                mode = fp::TransportMode::Http;
                break;
            case fp::TransportMode::Http:
                mode = fp::TransportMode::Mqtt;
                break;
            case fp::TransportMode::Mqtt:
            default:
                mode = fp::TransportMode::Auto;
                break;
        }
        state.config().transport.mode = mode;
        state.touch();
        state.requestSave();
    }
    net::requestTransportReload();
    lv_label_set_text_fmt(g_transportLabel, "Transport: %s", fp::transportModeName(mode));
}

void onOpenWifi(lv_event_t*) {
    noteInteraction();
    showWifiSetup();
}
void onOpenDevices(lv_event_t*) {
    noteInteraction();
    showDeviceList();
}
void onOpenDiagnostics(lv_event_t*) {
    noteInteraction();
    showDiagnostics();
}
void onRescan(lv_event_t*) {
    noteInteraction();
    net::requestDiscoveryScan();
    toast("Scanning for agents...");
}

}  // namespace

lv_obj_t* settingsScreen() { return g_settingsScreen; }
lv_obj_t* wifiScreen() { return g_wifiScreen; }
lv_obj_t* deviceListScreen() { return g_devicesScreen; }
lv_obj_t* diagnosticsScreen() { return g_diagScreen; }

void buildSettingsScreen() {
    g_settingsScreen = makeScreen();
    makeTitleBar(g_settingsScreen, "Settings", onBackToDashboard);

    lv_obj_t* body = lv_obj_create(g_settingsScreen);
    lv_obj_set_pos(body, 0, kHeaderH + 8);
    lv_obj_set_size(body, kScreenW, kScreenH - kHeaderH - 8);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    lv_obj_t* wifiBtn = makeButton(body, LV_SYMBOL_WIFI "  Wi-Fi setup", 0, 0, 148, 40);
    lv_obj_add_event_cb(wifiBtn, onOpenWifi, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* devicesBtn = makeButton(body, LV_SYMBOL_LIST "  Devices", 156, 0, 148, 40);
    lv_obj_add_event_cb(devicesBtn, onOpenDevices, LV_EVENT_CLICKED, nullptr);

    makeLabel(body, "Carousel", &lv_font_montserrat_12, kColorTextDim, 0, 48);
    g_carouselSwitch = lv_switch_create(body);
    lv_obj_set_pos(g_carouselSwitch, 64, 44);
    lv_obj_set_style_bg_color(g_carouselSwitch, kColorCardAlt, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_carouselSwitch, kColorAccent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(g_carouselSwitch, onCarouselToggled, LV_EVENT_VALUE_CHANGED, nullptr);

    g_carouselSlider = lv_slider_create(body);
    lv_obj_set_pos(g_carouselSlider, 130, 52);
    lv_obj_set_size(g_carouselSlider, 130, 10);
    lv_slider_set_range(g_carouselSlider, 3, 120);
    lv_obj_set_style_bg_color(g_carouselSlider, kColorCardAlt, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_carouselSlider, kColorAccent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_carouselSlider, kColorAccent, LV_PART_KNOB);
    lv_obj_add_event_cb(g_carouselSlider, onCarouselInterval, LV_EVENT_VALUE_CHANGED, nullptr);
    g_carouselValue = makeLabel(body, "10s", &lv_font_montserrat_12, kColorText, 268, 48);

    makeLabel(body, "Brightness", &lv_font_montserrat_12, kColorTextDim, 0, 84);
    g_brightnessSlider = lv_slider_create(body);
    lv_obj_set_pos(g_brightnessSlider, 130, 88);
    lv_obj_set_size(g_brightnessSlider, 130, 10);
    lv_slider_set_range(g_brightnessSlider, 5, 100);
    lv_obj_set_style_bg_color(g_brightnessSlider, kColorCardAlt, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_brightnessSlider, kColorAccent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_brightnessSlider, kColorAccent, LV_PART_KNOB);
    lv_obj_add_event_cb(g_brightnessSlider, onBrightness, LV_EVENT_VALUE_CHANGED, nullptr);
    g_brightnessValue = makeLabel(body, "80%", &lv_font_montserrat_12, kColorText, 268, 84);

    lv_obj_t* transportBtn = makeButton(body, "Transport: auto", 0, 108, 148, 40);
    lv_obj_add_event_cb(transportBtn, onTransportPressed, LV_EVENT_CLICKED, nullptr);
    g_transportLabel = lv_obj_get_child(transportBtn, 0);

    lv_obj_t* rescanBtn = makeButton(body, LV_SYMBOL_REFRESH "  Find agents", 156, 108, 148, 40);
    lv_obj_add_event_cb(rescanBtn, onRescan, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* diagBtn = makeButton(body, LV_SYMBOL_SETTINGS "  Diagnostics", 0, 156, 304, 40);
    lv_obj_add_event_cb(diagBtn, onOpenDiagnostics, LV_EVENT_CLICKED, nullptr);
}

void refreshSettingsScreen() {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    const fp::PanelConfig& config = state.config();
    if (config.carousel.enabled) {
        lv_obj_add_state(g_carouselSwitch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(g_carouselSwitch, LV_STATE_CHECKED);
    }
    lv_slider_set_value(g_carouselSlider, static_cast<int32_t>(config.carousel.intervalSeconds),
                        LV_ANIM_OFF);
    lv_label_set_text_fmt(g_carouselValue, "%lus", (unsigned long)config.carousel.intervalSeconds);
    lv_slider_set_value(g_brightnessSlider, config.display.brightness, LV_ANIM_OFF);
    lv_label_set_text_fmt(g_brightnessValue, "%u%%", config.display.brightness);
    lv_label_set_text_fmt(g_transportLabel, "Transport: %s",
                          fp::transportModeName(config.transport.mode));
}

// =============================================================== Wi-Fi

namespace {

void hideKeyboard() {
    g_promptOpen = false;
    if (g_keyboard != nullptr) {
        lv_keyboard_set_textarea(g_keyboard, nullptr);
        lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_connectPanel != nullptr) {
        lv_obj_add_flag(g_connectPanel, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_wifiList != nullptr) {
        lv_obj_clear_flag(g_wifiList, LV_OBJ_FLAG_HIDDEN);
    }
}

// Raising the keyboard is its own function because three things do it: opening the
// prompt, tapping the password field, and the keyboard's own hide button being
// pressed by accident.
void raiseKeyboard() {
    if (g_keyboard == nullptr || g_passwordArea == nullptr) {
        return;
    }
    lv_keyboard_set_mode(g_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(g_keyboard, g_passwordArea);
    lv_obj_clear_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    // The keyboard is created before some later widgets, so without this it can end
    // up behind the network list when the list is rebuilt by a scan.
    lv_obj_move_foreground(g_keyboard);
}

void onPasswordFieldTapped(lv_event_t*) {
    noteInteraction();
    raiseKeyboard();
}

void onCancelConnect(lv_event_t*) {
    noteInteraction();
    hideKeyboard();
}

void onRevealPassword(lv_event_t*) {
    noteInteraction();
    const bool hidden = lv_textarea_get_password_mode(g_passwordArea);
    lv_textarea_set_password_mode(g_passwordArea, !hidden);
    lv_label_set_text(lv_obj_get_child(g_revealButton, 0),
                      hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

void onConnectPressed(lv_event_t*) {
    noteInteraction();
    const char* password = lv_textarea_get_text(g_passwordArea);
    if (g_pendingSsid.empty()) {
        return;
    }
    net::wifi().connectTo(g_pendingSsid, password == nullptr ? "" : password, /*remember=*/true,
                          g_pendingHidden);
    // The password is wiped from the text area immediately; it lives only in the
    // configuration from here on and is never displayed again.
    lv_textarea_set_text(g_passwordArea, "");
    hideKeyboard();
    lv_label_set_text_fmt(g_wifiStatus, "Connecting to %s...", g_pendingSsid.c_str());
}

void showPasswordPrompt(const std::string& ssid, bool secure, bool hidden) {
    g_pendingSsid = ssid;
    g_pendingHidden = hidden;
    g_promptOpen = true;
    lv_label_set_text_fmt(g_connectTitle, "%s%s", ssid.c_str(), secure ? "" : "  (open)");
    lv_textarea_set_text(g_passwordArea, "");
    lv_obj_add_flag(g_wifiList, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_connectPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_connectPanel);
    // Always raise the keyboard, including for an open network. Deciding for the
    // user based on the scan's security flag was wrong: a mesh SSID reported as
    // open on one band, or an entry whose scan record has since been replaced,
    // left the prompt with no way to type at all.
    raiseKeyboard();
}

void onNetworkChosen(lv_event_t* event) {
    noteInteraction();
    lv_obj_t* button = lv_event_get_target(event);
    const char* text = lv_list_get_btn_text(g_wifiList, button);
    if (text == nullptr) {
        return;
    }
    // The row text is "<ssid>  -52 dBm" plus markers; the SSID is stored on the
    // button's user data to avoid parsing it back out.
    const char* ssid = static_cast<const char*>(lv_obj_get_user_data(button));
    if (ssid == nullptr) {
        return;
    }
    bool secure = true;
    for (const net::ScanEntry& entry : net::wifi().scanResults()) {
        if (entry.ssid == ssid) {
            secure = entry.secure;
            break;
        }
    }
    showPasswordPrompt(ssid, secure, false);
}

void onForgetAll(lv_event_t*) {
    noteInteraction();
    net::wifi().forgetAll();
    toast("All networks forgotten");
}

void onRefreshScan(lv_event_t*) {
    noteInteraction();
    net::wifi().requestScan();
    lv_label_set_text(g_wifiStatus, "Scanning...");
}

}  // namespace

void buildWifiScreen() {
    g_wifiScreen = makeScreen();
    makeTitleBar(g_wifiScreen, "Wi-Fi", onBackToSettings);

    lv_obj_t* refresh = lv_btn_create(g_wifiScreen);
    lv_obj_set_pos(refresh, 214, 1);
    lv_obj_set_size(refresh, 48, 30);
    lv_obj_set_style_bg_color(refresh, kColorCardAlt, LV_PART_MAIN);
    lv_obj_set_style_border_width(refresh, 0, LV_PART_MAIN);
    lv_obj_t* refreshLabel = lv_label_create(refresh);
    lv_label_set_text(refreshLabel, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(refreshLabel, kColorText, LV_PART_MAIN);
    lv_obj_center(refreshLabel);
    lv_obj_add_event_cb(refresh, onRefreshScan, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* forget = lv_btn_create(g_wifiScreen);
    lv_obj_set_pos(forget, 266, 1);
    lv_obj_set_size(forget, 52, 30);
    lv_obj_set_style_bg_color(forget, kColorCardAlt, LV_PART_MAIN);
    lv_obj_set_style_bg_color(forget, kColorCritical, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(forget, 0, LV_PART_MAIN);
    lv_obj_t* forgetLabel = lv_label_create(forget);
    lv_label_set_text(forgetLabel, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(forgetLabel, kColorText, LV_PART_MAIN);
    lv_obj_center(forgetLabel);
    lv_obj_add_event_cb(forget, onForgetAll, LV_EVENT_CLICKED, nullptr);

    g_wifiStatus = makeLabel(g_wifiScreen, "", &lv_font_montserrat_12, kColorTextDim, 6, 36);
    lv_obj_set_width(g_wifiStatus, 308);
    lv_label_set_long_mode(g_wifiStatus, LV_LABEL_LONG_DOT);

    g_wifiList = lv_list_create(g_wifiScreen);
    lv_obj_set_pos(g_wifiList, 4, 54);
    lv_obj_set_size(g_wifiList, 312, 182);
    lv_obj_set_style_bg_color(g_wifiList, kColorCard, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_wifiList, kColorBorder, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_wifiList, kColorText, LV_PART_MAIN);

    // ---- password panel, hidden until a network is chosen
    g_connectPanel = lv_obj_create(g_wifiScreen);
    lv_obj_set_pos(g_connectPanel, 4, 52);
    // 52 + 74 = 126, and the keyboard starts at 130. The two must not overlap: an
    // overlapping keyboard swallows the taps meant for the Connect button.
    lv_obj_set_size(g_connectPanel, 312, 74);
    lv_obj_set_style_bg_color(g_connectPanel, kColorCard, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_connectPanel, kColorBorder, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_connectPanel, 6, LV_PART_MAIN);
    lv_obj_clear_flag(g_connectPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_connectPanel, LV_OBJ_FLAG_HIDDEN);

    g_connectTitle = makeLabel(g_connectPanel, "", &lv_font_montserrat_14, kColorText, 0, 0);
    g_passwordArea = lv_textarea_create(g_connectPanel);
    lv_obj_set_pos(g_passwordArea, 0, 20);
    lv_obj_set_size(g_passwordArea, 190, 36);
    lv_textarea_set_one_line(g_passwordArea, true);
    lv_textarea_set_password_mode(g_passwordArea, true);
    lv_textarea_set_placeholder_text(g_passwordArea, "password");
    lv_obj_set_style_bg_color(g_passwordArea, kColorCardAlt, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_passwordArea, kColorText, LV_PART_MAIN);
    // Tapping the field brings the keyboard back if it was dismissed with its own
    // hide button, which is the first thing anyone tries.
    lv_obj_add_event_cb(g_passwordArea, onPasswordFieldTapped, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g_passwordArea, onPasswordFieldTapped, LV_EVENT_FOCUSED, nullptr);

    g_revealButton = lv_btn_create(g_connectPanel);
    lv_obj_set_pos(g_revealButton, 196, 20);
    lv_obj_set_size(g_revealButton, 40, 36);
    lv_obj_set_style_bg_color(g_revealButton, kColorCardAlt, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_revealButton, 0, LV_PART_MAIN);
    lv_obj_t* eye = lv_label_create(g_revealButton);
    lv_label_set_text(eye, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(eye, kColorText, LV_PART_MAIN);
    lv_obj_center(eye);
    lv_obj_add_event_cb(g_revealButton, onRevealPassword, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* connect = makeButton(g_connectPanel, LV_SYMBOL_OK, 242, 20, 26, 36);
    lv_obj_set_size(connect, 26, 36);
    lv_obj_add_event_cb(connect, onConnectPressed, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancel = makeButton(g_connectPanel, LV_SYMBOL_CLOSE, 272, 20, 26, 36);
    lv_obj_set_size(cancel, 26, 36);
    lv_obj_add_event_cb(cancel, onCancelConnect, LV_EVENT_CLICKED, nullptr);

    g_keyboard = lv_keyboard_create(g_wifiScreen);
    lv_obj_set_pos(g_keyboard, 0, 130);
    lv_obj_set_size(g_keyboard, kScreenW, 110);
    lv_obj_set_style_bg_color(g_keyboard, kColorCard, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_keyboard, 2, LV_PART_MAIN);
    lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    // LVGL's own handler hides the keyboard on Ready and Cancel; mirror that into
    // our prompt state so the list comes back instead of leaving a dead panel.
    lv_obj_add_event_cb(g_keyboard, [](lv_event_t* event) {
        const lv_event_code_t code = lv_event_get_code(event);
        if (code == LV_EVENT_READY) {
            onConnectPressed(event);
        } else if (code == LV_EVENT_CANCEL) {
            hideKeyboard();
        }
    }, LV_EVENT_ALL, nullptr);
}

void refreshWifiScreen() {
    net::WifiManager& manager = net::wifi();

    app::AppState& state = app::state();
    char status[160];
    {
        app::AppState::Lock lock(state);
        const app::RuntimeStatus& runtime = state.status();
        if (runtime.wifi == app::WifiPhase::Connected) {
            snprintf(status, sizeof(status), "Connected to %s  %s  %d dBm  |  %u saved",
                     runtime.ssid, runtime.ip, runtime.rssi,
                     (unsigned)state.config().wifi.networks.size());
        } else if (runtime.wifi == app::WifiPhase::Portal) {
            snprintf(status, sizeof(status), "Setup AP %s at %s", runtime.apSsid, runtime.apIp);
        } else {
            snprintf(status, sizeof(status), "%s", manager.lastMessage());
        }
    }
    lv_label_set_text(g_wifiStatus, status);

    // Never rebuild while the password prompt is open. The rebuild frees the row
    // the prompt was opened from, and on a 320x240 panel it also reflows under the
    // user's finger mid-password.
    if (g_promptOpen) {
        return;
    }

    // Rebuild the list only when a scan actually completed; rebuilding on every
    // refresh would fight the user's scrolling.
    const bool completed = manager.takeScanResult();
    if (!completed && g_lastWifiListMs != 0) {
        return;
    }
    g_lastWifiListMs = millis();

    releaseListUserData(g_wifiList);
    lv_obj_clean(g_wifiList);
    for (const net::ScanEntry& entry : manager.scanResults()) {
        char text[80];
        snprintf(text, sizeof(text), "%s   %d dBm%s%s", entry.ssid.c_str(), (int)entry.rssi,
                 entry.secure ? "  " LV_SYMBOL_CLOSE : "  " LV_SYMBOL_MINUS,
                 entry.known ? "  " LV_SYMBOL_OK : "");
        lv_obj_t* button =
            lv_list_add_btn(g_wifiList, entry.secure ? LV_SYMBOL_WIFI : LV_SYMBOL_WIFI, text);
        lv_obj_set_style_bg_color(button, kColorCard, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, kColorAccent, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_text_color(button, kColorText, LV_PART_MAIN);
        lv_obj_set_height(button, 40);  // finger-sized rows
        // strdup: LVGL owns the button for the lifetime of the list, and the list is
        // cleaned (freeing user data with it) on the next rebuild.
        lv_obj_set_user_data(button, strdup(entry.ssid.c_str()));
        lv_obj_add_event_cb(button, onNetworkChosen, LV_EVENT_CLICKED, nullptr);
    }
    if (manager.scanResults().empty()) {
        lv_obj_t* button = lv_list_add_btn(g_wifiList, nullptr,
                                           manager.scanInProgress() ? "Scanning..."
                                                                    : "No networks found");
        lv_obj_set_style_text_color(button, kColorTextDim, LV_PART_MAIN);
    }
}

// ============================================================= devices

namespace {

void onDeviceChosen(lv_event_t* event) {
    noteInteraction();
    lv_obj_t* button = lv_event_get_target(event);
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
    setPageIndex(index);
    showDashboard();
}

void onApproveDiscovered(lv_event_t* event) {
    noteInteraction();
    lv_obj_t* button = lv_event_get_target(event);
    // The row stores its index in the pending list rather than a heap copy of the
    // id, so lv_obj_clean() on the next refresh cannot leak or double-free.
    const size_t index = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
    app::AppState& state = app::state();
    bool ok = false;
    {
        app::AppState::Lock lock(state);
        if (index >= state.devices().pending().size()) {
            toast("That device is no longer in the discovered list");
            return;
        }
        const std::string id = state.devices().pending()[index].id;
        ok = state.devices().approve(id);
        if (ok) {
            state.status().discoveredCount = state.devices().pending().size();
            state.touch();
            state.requestSave();
        }
    }
    toast(ok ? "Device added" : "Could not add device");
    refreshDeviceListScreen();
}

}  // namespace

void buildDeviceListScreen() {
    g_devicesScreen = makeScreen();
    makeTitleBar(g_devicesScreen, "Devices", onBackToSettings);

    g_deviceList = lv_list_create(g_devicesScreen);
    lv_obj_set_pos(g_deviceList, 4, 38);
    lv_obj_set_size(g_deviceList, 312, 198);
    lv_obj_set_style_bg_color(g_deviceList, kColorCard, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_deviceList, kColorBorder, LV_PART_MAIN);
}

void refreshDeviceListScreen() {
    // Every row here stores an index cast to a pointer, never an allocation, so
    // lv_obj_clean() is sufficient and there is nothing to free.
    lv_obj_clean(g_deviceList);

    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    const uint32_t now = millis();
    const fp::Thresholds& thresholds = state.config().thresholds;
    const std::vector<size_t> order = state.devices().visibleOrder();

    for (size_t position = 0; position < order.size(); ++position) {
        const fp::DeviceState& device = state.devices().all()[order[position]];
        const fp::Freshness freshness =
            device.announcedOffline
                ? fp::Freshness::Offline
                : fp::freshnessFor(device.ageSeconds(now), device.everReceived, thresholds);
        char text[96];
        snprintf(text, sizeof(text), "%s   %s   %s", device.config.label().c_str(),
                 fp::freshnessName(freshness),
                 device.everReceived ? fp::formatAge(device.ageSeconds(now)).c_str() : "");
        lv_obj_t* button = lv_list_add_btn(g_deviceList, LV_SYMBOL_HOME, text);
        lv_obj_set_style_bg_color(button, kColorCard, LV_PART_MAIN);
        lv_obj_set_style_text_color(button, colourFor(freshness), LV_PART_MAIN);
        lv_obj_set_height(button, 40);
        lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(position)));
        lv_obj_add_event_cb(button, onDeviceChosen, LV_EVENT_CLICKED, nullptr);
    }

    if (!state.devices().pending().empty()) {
        lv_obj_t* header = lv_list_add_text(g_deviceList, "Discovered - tap to add");
        lv_obj_set_style_text_color(header, kColorAccent, LV_PART_MAIN);
        const std::vector<fp::DeviceConfig>& pendingList = state.devices().pending();
        for (size_t index = 0; index < pendingList.size(); ++index) {
            const fp::DeviceConfig& pending = pendingList[index];
            char text[96];
            snprintf(text, sizeof(text), "%s   %s", pending.name.c_str(), pending.baseUrl.c_str());
            lv_obj_t* button = lv_list_add_btn(g_deviceList, LV_SYMBOL_PLUS, text);
            lv_obj_set_style_bg_color(button, kColorCardAlt, LV_PART_MAIN);
            lv_obj_set_style_text_color(button, kColorText, LV_PART_MAIN);
            lv_obj_set_height(button, 40);
            lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
            lv_obj_add_event_cb(button, onApproveDiscovered, LV_EVENT_CLICKED, nullptr);
        }
    }

    if (order.empty() && state.devices().pending().empty()) {
        lv_obj_t* button = lv_list_add_btn(g_deviceList, nullptr,
                                           "No devices. Use the web interface to add one.");
        lv_obj_set_style_text_color(button, kColorTextDim, LV_PART_MAIN);
    }
}

// ========================================================= diagnostics

namespace {

void onRestartPressed(lv_event_t*) {
    noteInteraction();
    toast("Restarting...");
    lv_timer_handler();
    delay(300);
    ESP.restart();
}

void onFactoryResetConfirm(lv_event_t* event) {
    lv_obj_t* box = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    const uint16_t clicked = lv_msgbox_get_active_btn(box);
    lv_msgbox_close(box);
    if (clicked != 0) {
        return;  // "Cancel"
    }
    app::state().factoryReset();
    toast("Erased. Restarting...");
    lv_timer_handler();
    delay(500);
    ESP.restart();
}

void onFactoryResetPressed(lv_event_t*) {
    noteInteraction();
    static const char* buttons[] = {"Erase", "Cancel", ""};
    lv_obj_t* box = lv_msgbox_create(nullptr, "Factory reset",
                                     "This erases every setting, device and saved Wi-Fi network.",
                                     buttons, false);
    lv_obj_set_width(box, 290);
    lv_obj_set_style_bg_color(box, kColorCard, LV_PART_MAIN);
    lv_obj_set_style_text_color(box, kColorText, LV_PART_MAIN);
    lv_obj_center(box);
    lv_obj_add_event_cb(box, onFactoryResetConfirm, LV_EVENT_VALUE_CHANGED, box);
}

}  // namespace

void buildDiagnosticsScreen() {
    g_diagScreen = makeScreen();
    makeTitleBar(g_diagScreen, "Diagnostics", onBackToSettings);

    g_diagText = makeLabel(g_diagScreen, "", &lv_font_montserrat_12, kColorText, 8, 38);
    lv_obj_set_width(g_diagText, 304);
    lv_label_set_long_mode(g_diagText, LV_LABEL_LONG_WRAP);

    lv_obj_t* restart = makeButton(g_diagScreen, LV_SYMBOL_REFRESH "  Restart", 8, 192, 148, 40);
    lv_obj_add_event_cb(restart, onRestartPressed, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* reset = makeButton(g_diagScreen, LV_SYMBOL_TRASH "  Factory", 164, 192, 148, 40);
    lv_obj_set_style_bg_color(reset, kColorCritical, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(reset, onFactoryResetPressed, LV_EVENT_CLICKED, nullptr);
}

void refreshDiagnosticsScreen() {
    app::AppState& state = app::state();
    char text[640];
    {
        app::AppState::Lock lock(state);
        const app::RuntimeStatus& status = state.status();
        snprintf(text, sizeof(text),
                 "Firmware   %s  (%s)\n"
                 "Uptime     %s\n"
                 "Free heap  %u B  (min %u B)\n"
                 "Reset      %s   boot #%lu\n"
                 "Config     %s\n"
                 "Wi-Fi      %s  %s  %d dBm\n"
                 "Host       %s.local\n"
                 "Transport  %s (mode %s)\n"
                 "MQTT       %s   msgs %lu\n"
                 "HTTP       %lu polls / %lu failures\n"
                 "Devices    %u active, %u pending\n"
                 "Log        port %u, %u client(s)",
                 FP_FIRMWARE_VERSION, FP_PRODUCT_NAME,
                 fp::formatUptime(millis() / 1000).c_str(), (unsigned)ESP.getFreeHeap(),
                 (unsigned)ESP.getMinFreeHeap(), status.resetReason,
                 (unsigned long)status.bootCount, status.configStatus,
                 app::wifiPhaseName(status.wifi), status.ip[0] ? status.ip : "-", status.rssi,
                 state.config().wifi.hostname.c_str(), net::activeTransportName(),
                 fp::transportModeName(state.config().transport.mode),
                 status.mqttConnected ? "connected" : "disconnected",
                 (unsigned long)status.mqttMessages, (unsigned long)status.httpPolls,
                 (unsigned long)status.httpFailures, (unsigned)state.devices().size(),
                 (unsigned)state.devices().pending().size(),
                 state.config().logging.telnetPort, fplog::consoleClients());
    }
    lv_label_set_text(g_diagText, text);
}

// ========================================================== navigation

void showSettings() {
    refreshSettingsScreen();
    lv_scr_load_anim(g_settingsScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 140, 0, false);
}

void showWifiSetup() {
    // Entering the screen always starts on the network list, never on a stale
    // password prompt left over from a previous visit.
    hideKeyboard();
    net::wifi().requestScan();
    g_lastWifiListMs = 0;
    refreshWifiScreen();
    lv_scr_load_anim(g_wifiScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 140, 0, false);
}

void showDeviceList() {
    refreshDeviceListScreen();
    lv_scr_load_anim(g_devicesScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 140, 0, false);
}

void showDiagnostics() {
    refreshDiagnosticsScreen();
    lv_scr_load_anim(g_diagScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 140, 0, false);
}

}  // namespace ui
