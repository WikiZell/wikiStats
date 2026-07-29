#include "ui.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include <cstdio>

#include "../app_state.h"
#include "../hal/display_hal.h"
#include "../log.h"
#include "../net/net_task.h"
#include "../net/screenshot.h"
#include "../net/wifi_manager.h"
#include "fp_carousel.h"
#include "fp_gesture.h"
#include "fp_units.h"
#include "ui_internal.h"

namespace ui {

namespace {

constexpr const char* kTag = "ui";

// 40 lines of a 320-wide panel: 25 KiB of DMA-capable RAM. Two buffers would be
// smoother but this board has no PSRAM and the Wi-Fi stack needs the headroom more
// than the UI needs tear-free scrolling.
constexpr uint32_t kDrawBufferLines = 40;

lv_disp_draw_buf_t g_drawBuffer;
lv_color_t* g_pixels = nullptr;
lv_disp_drv_t g_dispDriver;
lv_indev_drv_t g_indevDriver;
lv_indev_t* g_indev = nullptr;

hal::TouchPoint g_touch;
fp::GestureDetector g_gestures;
fp::Carousel g_carousel;

lv_obj_t* g_dashboard = nullptr;
lv_obj_t* g_toast = nullptr;

int g_pageIndex = 0;
uint32_t g_lastInteractionMs = 0;
uint32_t g_lastRevision = 0xFFFFFFFF;
uint32_t g_lastRenderMs = 0;
bool g_dimmed = false;

// ---- dashboard widgets, created once and updated in place
struct Dashboard {
    lv_obj_t* header = nullptr;
    lv_obj_t* nameLabel = nullptr;
    lv_obj_t* platformLabel = nullptr;
    lv_obj_t* stateLabel = nullptr;
    lv_obj_t* wifiLabel = nullptr;
    lv_obj_t* pageLabel = nullptr;

    lv_obj_t* cpuCard = nullptr;
    lv_obj_t* cpuValue = nullptr;
    lv_obj_t* cpuTag = nullptr;
    lv_obj_t* cpuTemp = nullptr;
    lv_obj_t* cpuTempTag = nullptr;
    lv_obj_t* cpuChart = nullptr;
    lv_chart_series_t* cpuSeries = nullptr;

    lv_obj_t* ramCard = nullptr;
    lv_obj_t* ramValue = nullptr;
    lv_obj_t* ramTag = nullptr;
    lv_obj_t* ramBar = nullptr;
    lv_obj_t* ramDetail = nullptr;
    lv_obj_t* ramChart = nullptr;
    lv_chart_series_t* ramSeries = nullptr;

    lv_obj_t* diskCard = nullptr;
    lv_obj_t* diskValue = nullptr;
    lv_obj_t* diskTag = nullptr;
    lv_obj_t* diskBar = nullptr;
    lv_obj_t* diskDetail = nullptr;
    lv_obj_t* diskFree = nullptr;

    lv_obj_t* netCard = nullptr;
    lv_obj_t* rxLabel = nullptr;
    lv_obj_t* txLabel = nullptr;

    lv_obj_t* statusStrip = nullptr;
    lv_obj_t* uptimeLabel = nullptr;
    lv_obj_t* ageLabel = nullptr;

    lv_obj_t* dots[12] = {nullptr};
    lv_obj_t* gear = nullptr;
    lv_obj_t* prevButton = nullptr;
    lv_obj_t* nextButton = nullptr;
    lv_obj_t* emptyLabel = nullptr;
};
Dashboard g_ui;

// ---- dashboard geometry, in one place so the cards and the tap hit-test cannot
// drift apart.
constexpr lv_coord_t kRow1Y = 30;
constexpr lv_coord_t kRow1H = 78;
constexpr lv_coord_t kRow2Y = 112;
constexpr lv_coord_t kRow2H = 62;
constexpr lv_coord_t kStripY = 177;
constexpr lv_coord_t kStripH = 28;
constexpr lv_coord_t kFooterY = 207;
constexpr lv_coord_t kFooterHeight = 31;
constexpr uint8_t kMaxDots = 12;
constexpr lv_coord_t kCardW = 152;
constexpr lv_coord_t kLeftX = 4;
constexpr lv_coord_t kRightX = 164;

struct Rect {
    lv_coord_t x;
    lv_coord_t y;
    lv_coord_t w;
    lv_coord_t h;
    bool contains(int16_t px, int16_t py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

constexpr Rect kHeaderRect{0, 0, kScreenW, kHeaderH};
constexpr Rect kCpuRect{kLeftX, kRow1Y, kCardW, kRow1H};
constexpr Rect kRamRect{kRightX, kRow1Y, kCardW, kRow1H};
constexpr Rect kDiskRect{kLeftX, kRow2Y, kCardW, kRow2H};
constexpr Rect kNetRect{kRightX, kRow2Y, kCardW, kRow2H};
constexpr Rect kDotsRect{52, kFooterY, 160, kFooterHeight};

// ------------------------------------------------------------ LVGL glue

void flushCallback(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* colours) {
    hal::display().flush(area->x1, area->y1, area->x2, area->y2,
                         reinterpret_cast<uint16_t*>(colours));
    // Same pixels, second destination: while a screenshot is being captured the
    // band goes straight to the socket instead of being buffered, because a full
    // frame is 150 KiB and this board does not have it to spare.
    shot::writeBand(area->x1, area->y1, area->x2, area->y2,
                    reinterpret_cast<const uint16_t*>(colours));
    lv_disp_flush_ready(driver);
}

void touchCallback(lv_indev_drv_t*, lv_indev_data_t* data) {
    // The hardware read happens once per tick() in the UI task; this only reports
    // the cached value so LVGL and the gesture detector always agree.
    data->state = g_touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = g_touch.x;
    data->point.y = g_touch.y;
}

// ------------------------------------------------------------- helpers

int visibleCount() {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    return static_cast<int>(state.devices().visibleOrder().size());
}

// Copies what the dashboard needs out of shared state in one short lock, so the
// render itself never holds the mutex while touching LVGL.
struct PageSnapshot {
    bool valid = false;
    std::string label;
    std::string platform;
    fp::Freshness freshness = fp::Freshness::Never;
    fp::Telemetry telemetry;
    uint8_t cpuHistory[fp::kHistoryPoints];
    uint8_t ramHistory[fp::kHistoryPoints];
    size_t cpuCount = 0;
    size_t ramCount = 0;
    uint32_t ageSeconds = 0;
    fp::Thresholds thresholds;
    int pageIndex = 0;
    int pageCount = 0;
    std::string lastError;
};

bool snapshotPage(PageSnapshot& out) {
    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    const std::vector<size_t> order = state.devices().visibleOrder();
    out.pageCount = static_cast<int>(order.size());
    out.thresholds = state.config().thresholds;
    if (order.empty()) {
        out.valid = false;
        return false;
    }
    if (g_pageIndex < 0 || g_pageIndex >= static_cast<int>(order.size())) {
        g_pageIndex = 0;
    }
    out.pageIndex = g_pageIndex;

    const fp::DeviceState& device = state.devices().all()[order[static_cast<size_t>(g_pageIndex)]];
    out.label = device.config.label();
    out.platform = device.config.platform;
    out.telemetry = device.latest;
    out.ageSeconds = device.ageSeconds(millis());
    out.lastError = device.lastError;
    out.freshness = device.announcedOffline
                        ? fp::Freshness::Offline
                        : fp::freshnessFor(out.ageSeconds, device.everReceived, out.thresholds);
    out.cpuCount = device.cpuHistory.size();
    out.ramCount = device.ramHistory.size();
    for (size_t i = 0; i < out.cpuCount; ++i) {
        out.cpuHistory[i] = device.cpuHistory.at(i);
    }
    for (size_t i = 0; i < out.ramCount; ++i) {
        out.ramHistory[i] = device.ramHistory.at(i);
    }
    out.valid = true;
    return true;
}

const char* platformSymbol(const std::string& platform) {
    // LVGL's built-in symbol font has no OS logos; these read clearly at 14 px and
    // cost no extra flash.
    if (platform == "windows") {
        return LV_SYMBOL_DRIVE;
    }
    if (platform == "macos") {
        return LV_SYMBOL_IMAGE;
    }
    return LV_SYMBOL_HOME;
}

const char* wifiSymbolFor(int8_t rssi, bool connected) {
    if (!connected) {
        return LV_SYMBOL_CLOSE;
    }
    return LV_SYMBOL_WIFI;
}

void setChartFrom(lv_obj_t* chart, lv_chart_series_t* series, const uint8_t* history,
                  size_t count) {
    if (chart == nullptr || series == nullptr) {
        return;
    }
    // Only plot the samples that exist. Fixing the point count at the ring's
    // capacity left a freshly booted panel drawing its trace squashed into the
    // left sixth of the card, which reads as a rendering fault rather than as a
    // history that has not filled up yet.
    if (count < 2) {
        lv_chart_set_point_count(chart, 1);
        series->y_points[0] = LV_CHART_POINT_NONE;
        lv_chart_refresh(chart);
        return;
    }
    lv_chart_set_point_count(chart, static_cast<uint16_t>(count));
    for (size_t i = 0; i < count; ++i) {
        const uint8_t value = history[i];
        series->y_points[i] =
            value == fp::kHistoryEmpty ? LV_CHART_POINT_NONE : static_cast<lv_coord_t>(value);
    }
    lv_chart_refresh(chart);
}

// ------------------------------------------------------------- dialogs

void closeDialog(lv_event_t* event) {
    lv_obj_t* box = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    if (box != nullptr) {
        lv_msgbox_close(box);
    }
}

void showDialog(const char* title, const char* body) {
    static const char* buttons[] = {"Close", ""};
    lv_obj_t* box = lv_msgbox_create(nullptr, title, body, buttons, false);
    lv_obj_set_width(box, 300);
    lv_obj_set_style_bg_color(box, kColorCard, LV_PART_MAIN);
    lv_obj_set_style_text_color(box, kColorText, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, kColorBorder, LV_PART_MAIN);
    lv_obj_center(box);
    lv_obj_add_event_cb(box, closeDialog, LV_EVENT_VALUE_CHANGED, box);
    noteInteraction();
}

void showDeviceInfo(lv_event_t*) {
    PageSnapshot page;
    if (!snapshotPage(page)) {
        return;
    }
    const fp::Telemetry& t = page.telemetry;
    char body[512];
    snprintf(body, sizeof(body),
             "ID        %s\n"
             "Host      %s\n"
             "Platform  %s\n"
             "OS        %s %s\n"
             "Kernel    %s\n"
             "Arch      %s\n"
             "Model     %s\n"
             "Agent     %s\n"
             "Address   %s\n"
             "State     %s (%s ago)\n"
             "%s",
             t.deviceId.empty() ? "--" : t.deviceId.c_str(),
             t.hostname.empty() ? "--" : t.hostname.c_str(),
             t.platform.empty() ? "--" : t.platform.c_str(),
             t.osName.empty() ? "--" : t.osName.c_str(), t.osVersion.c_str(),
             t.kernel.empty() ? "--" : t.kernel.c_str(),
             t.architecture.empty() ? "--" : t.architecture.c_str(),
             t.hardwareModel.empty() ? "--" : t.hardwareModel.c_str(),
             t.agentVersion.empty() ? "--" : t.agentVersion.c_str(),
             t.primaryAddress.empty() ? "--" : t.primaryAddress.c_str(),
             fp::freshnessName(page.freshness), fp::formatAge(page.ageSeconds).c_str(),
             page.lastError.empty() ? "" : page.lastError.c_str());
    showDialog(page.label.c_str(), body);
}

void showCpuDetail(lv_event_t*) {
    PageSnapshot page;
    if (!snapshotPage(page)) {
        return;
    }
    const fp::Telemetry& t = page.telemetry;
    char cores[160] = {0};
    size_t offset = 0;
    for (size_t i = 0; i < t.perCorePercent.size() && offset + 12 < sizeof(cores); ++i) {
        offset += static_cast<size_t>(snprintf(cores + offset, sizeof(cores) - offset, "%s%.0f%%",
                                               i ? " " : "", t.perCorePercent[i]));
    }
    char body[420];
    snprintf(body, sizeof(body),
             "Usage      %s\n"
             "Temp       %s\n"
             "Frequency  %s\n"
             "Cores      %d physical / %d logical\n"
             "Load       %.2f  %.2f  %.2f\n"
             "Per core   %s",
             t.cpuPercent.has ? fp::formatPercent(t.cpuPercent.value).c_str() : fp::kNoValue,
             t.cpuTemperature.has ? fp::formatTemperature(t.cpuTemperature.value).c_str()
                                  : fp::kNoValue,
             t.frequencyMhz.has ? fp::formatFrequency(t.frequencyMhz.value).c_str() : fp::kNoValue,
             t.physicalCores.orDefault(0), t.logicalCores.orDefault(0),
             static_cast<double>(t.load1.orDefault(0.0f)),
             static_cast<double>(t.load5.orDefault(0.0f)),
             static_cast<double>(t.load15.orDefault(0.0f)),
             cores[0] ? cores : fp::kNoValue);
    showDialog("CPU", body);
}

void showMemoryDetail(lv_event_t*) {
    PageSnapshot page;
    if (!snapshotPage(page)) {
        return;
    }
    const fp::Telemetry& t = page.telemetry;
    char body[420];
    snprintf(body, sizeof(body),
             "Used       %s\n"
             "Available  %s\n"
             "Total      %s\n"
             "Usage      %s\n\n"
             "Swap used  %s\n"
             "Swap total %s\n"
             "Swap usage %s",
             t.memUsed.has ? fp::formatBytes(static_cast<double>(t.memUsed.value)).c_str()
                           : fp::kNoValue,
             t.memAvailable.has ? fp::formatBytes(static_cast<double>(t.memAvailable.value)).c_str()
                                : fp::kNoValue,
             t.memTotal.has ? fp::formatBytes(static_cast<double>(t.memTotal.value)).c_str()
                            : fp::kNoValue,
             t.memPercent.has ? fp::formatPercent(t.memPercent.value).c_str() : fp::kNoValue,
             t.swapUsed.has ? fp::formatBytes(static_cast<double>(t.swapUsed.value)).c_str()
                            : fp::kNoValue,
             t.swapTotal.has ? fp::formatBytes(static_cast<double>(t.swapTotal.value)).c_str()
                             : fp::kNoValue,
             t.swapPercent.has ? fp::formatPercent(t.swapPercent.value).c_str() : fp::kNoValue);
    showDialog("Memory", body);
}

void showStorageDetail(lv_event_t*) {
    PageSnapshot page;
    if (!snapshotPage(page)) {
        return;
    }
    const fp::Telemetry& t = page.telemetry;
    char body[512];
    int offset = snprintf(body, sizeof(body),
                          "Used   %s\n"
                          "Free   %s\n"
                          "Total  %s\n"
                          "Usage  %s\n",
                          t.diskUsed.has
                              ? fp::formatBytes(static_cast<double>(t.diskUsed.value)).c_str()
                              : fp::kNoValue,
                          t.diskFree.has
                              ? fp::formatBytes(static_cast<double>(t.diskFree.value)).c_str()
                              : fp::kNoValue,
                          t.diskTotal.has
                              ? fp::formatBytes(static_cast<double>(t.diskTotal.value)).c_str()
                              : fp::kNoValue,
                          t.diskPercent.has ? fp::formatPercent(t.diskPercent.value).c_str()
                                            : fp::kNoValue);
    for (const fp::MountInfo& mount : t.mounts) {
        if (offset <= 0 || static_cast<size_t>(offset) >= sizeof(body) - 40) {
            break;
        }
        offset += snprintf(body + offset, sizeof(body) - static_cast<size_t>(offset),
                           "\n%s  %s of %s", mount.mountpoint.c_str(),
                           mount.usedBytes.has
                               ? fp::formatBytes(static_cast<double>(mount.usedBytes.value)).c_str()
                               : fp::kNoValue,
                           mount.totalBytes.has
                               ? fp::formatBytes(static_cast<double>(mount.totalBytes.value)).c_str()
                               : fp::kNoValue);
    }
    showDialog("Storage", body);
}

void showNetworkDetail(lv_event_t*) {
    PageSnapshot page;
    if (!snapshotPage(page)) {
        return;
    }
    const fp::Telemetry& t = page.telemetry;
    char body[420];
    snprintf(body, sizeof(body),
             "Interface  %s\n"
             "Address    %s\n\n"
             "Download   %s\n"
             "Upload     %s\n\n"
             "Received   %s\n"
             "Sent       %s",
             t.primaryInterface.empty() ? "--" : t.primaryInterface.c_str(),
             t.primaryAddress.empty() ? "--" : t.primaryAddress.c_str(),
             t.rxRate.has ? fp::formatRate(t.rxRate.value).c_str() : fp::kNoValue,
             t.txRate.has ? fp::formatRate(t.txRate.value).c_str() : fp::kNoValue,
             t.rxTotal.has ? fp::formatBytes(static_cast<double>(t.rxTotal.value)).c_str()
                           : fp::kNoValue,
             t.txTotal.has ? fp::formatBytes(static_cast<double>(t.txTotal.value)).c_str()
                           : fp::kNoValue);
    showDialog("Network", body);
}

void onGearClicked(lv_event_t*) {
    noteInteraction();
    showSettings();
}

void changePage(int delta, bool animate);
void renderDots(int count, int active);

void onPrevPage(lv_event_t*) {
    noteInteraction();
    changePage(-1, true);
}

void onNextPage(lv_event_t*) {
    noteInteraction();
    changePage(+1, true);
}

lv_obj_t* makeNavButton(lv_obj_t* parent, const char* symbol, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w) {
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, kFooterHeight);
    lv_obj_set_style_bg_color(button, kColorCard, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, kColorAccent, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, kColorText, LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

// ------------------------------------------------------------ dashboard

void buildDashboard() {
    g_dashboard = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(g_dashboard, kColorBackground, LV_PART_MAIN);
    lv_obj_clear_flag(g_dashboard, LV_OBJ_FLAG_SCROLLABLE);

    // ---- header
    g_ui.header = lv_obj_create(g_dashboard);
    lv_obj_set_pos(g_ui.header, 0, 0);
    lv_obj_set_size(g_ui.header, kScreenW, kHeaderH);
    lv_obj_set_style_bg_color(g_ui.header, kColorCard, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_ui.header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_ui.header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_ui.header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(g_ui.header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_ui.header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_ui.header, showDeviceInfo, LV_EVENT_CLICKED, nullptr);

    g_ui.platformLabel = makeLabel(g_ui.header, LV_SYMBOL_HOME, &lv_font_montserrat_14,
                                   kColorAccent, 6, 5);
    g_ui.nameLabel = makeLabel(g_ui.header, "WikiStats", &lv_font_montserrat_16, kColorText, 26, 3);
    lv_label_set_long_mode(g_ui.nameLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_ui.nameLabel, 150);
    g_ui.stateLabel = makeLabel(g_ui.header, "waiting", &lv_font_montserrat_12, kColorOffline,
                                182, 7);
    g_ui.wifiLabel = makeLabel(g_ui.header, LV_SYMBOL_WIFI, &lv_font_montserrat_14, kColorAccent,
                               258, 5);
    g_ui.pageLabel = makeLabel(g_ui.header, "0/0", &lv_font_montserrat_12, kColorTextDim, 284, 7);

    // Cards are NOT LVGL-clickable. LVGL fires CLICKED whenever a press and its
    // release land inside the same object, and a 45 px swipe fits comfortably
    // inside a 152 px card - so swiping across a card used to open its dialog.
    // Taps are dispatched by hit-test in processTouch() instead, from a gesture
    // the detector has already classified as a tap rather than a swipe.

    // ---- CPU card
    g_ui.cpuCard = makeCard(g_dashboard, kLeftX, kRow1Y, kCardW, kRow1H);
    makeLabel(g_ui.cpuCard, "CPU", &lv_font_montserrat_12, kColorTextDim, 0, 0);
    g_ui.cpuTag = makeLabel(g_ui.cpuCard, "", &lv_font_montserrat_12, kColorWarning, 108, 0);
    g_ui.cpuValue = makeLabel(g_ui.cpuCard, "--", &lv_font_montserrat_20, kColorText, 0, 13);
    g_ui.cpuTemp = makeLabel(g_ui.cpuCard, "--", &lv_font_montserrat_14, kColorTextDim, 84, 16);
    g_ui.cpuTempTag = makeLabel(g_ui.cpuCard, "", &lv_font_montserrat_12, kColorWarning, 84, 33);
    g_ui.cpuChart = makeSparkline(g_ui.cpuCard, 0, 48, 142, 18, &g_ui.cpuSeries);

    // ---- RAM card
    // The sparkline is a full-width strip at the bottom, the same as CPU. It used
    // to sit beside the "used / total" text, where a nearly flat memory trace drew
    // what looked like a stray rule straight through the numbers.
    g_ui.ramCard = makeCard(g_dashboard, kRightX, kRow1Y, kCardW, kRow1H);
    makeLabel(g_ui.ramCard, "RAM", &lv_font_montserrat_12, kColorTextDim, 0, 0);
    g_ui.ramTag = makeLabel(g_ui.ramCard, "", &lv_font_montserrat_12, kColorWarning, 108, 0);
    g_ui.ramValue = makeLabel(g_ui.ramCard, "--", &lv_font_montserrat_20, kColorText, 0, 13);
    g_ui.ramDetail = makeLabel(g_ui.ramCard, "--", &lv_font_montserrat_12, kColorTextDim, 54, 20);
    g_ui.ramBar = makeBar(g_ui.ramCard, 0, 40, 142, 6);
    g_ui.ramChart = makeSparkline(g_ui.ramCard, 0, 50, 142, 16, &g_ui.ramSeries);

    // ---- storage card
    g_ui.diskCard = makeCard(g_dashboard, kLeftX, kRow2Y, kCardW, kRow2H);
    makeLabel(g_ui.diskCard, "DISK", &lv_font_montserrat_12, kColorTextDim, 0, 1);
    g_ui.diskValue = makeLabel(g_ui.diskCard, "--", &lv_font_montserrat_16, kColorText, 40, -2);
    g_ui.diskTag = makeLabel(g_ui.diskCard, "", &lv_font_montserrat_12, kColorWarning, 108, 1);
    // The bar sits clear of the descenders of the 16 px value above it; at y=15 it
    // clipped the bottom of "DISK 54%".
    g_ui.diskBar = makeBar(g_ui.diskCard, 0, 19, 142, 5);
    g_ui.diskDetail = makeLabel(g_ui.diskCard, "--", &lv_font_montserrat_12, kColorTextDim, 0, 27);
    g_ui.diskFree = makeLabel(g_ui.diskCard, "--", &lv_font_montserrat_12, kColorTextDim, 0, 38);

    // ---- network card
    g_ui.netCard = makeCard(g_dashboard, kRightX, kRow2Y, kCardW, kRow2H);
    makeLabel(g_ui.netCard, "NET", &lv_font_montserrat_12, kColorTextDim, 0, 1);
    makeLabel(g_ui.netCard, LV_SYMBOL_DOWN, &lv_font_montserrat_12, kColorAccent, 0, 18);
    g_ui.rxLabel = makeLabel(g_ui.netCard, "--", &lv_font_montserrat_14, kColorText, 18, 16);
    makeLabel(g_ui.netCard, LV_SYMBOL_UP, &lv_font_montserrat_12, kColorAccent, 0, 36);
    g_ui.txLabel = makeLabel(g_ui.netCard, "--", &lv_font_montserrat_14, kColorText, 18, 34);

    // ---- status strip
    g_ui.statusStrip = makeCard(g_dashboard, kLeftX, kStripY, 312, kStripH);
    makeLabel(g_ui.statusStrip, "UP", &lv_font_montserrat_12, kColorTextDim, 0, 3);
    g_ui.uptimeLabel = makeLabel(g_ui.statusStrip, "--", &lv_font_montserrat_14, kColorText, 22, 1);
    makeLabel(g_ui.statusStrip, "UPDATED", &lv_font_montserrat_12, kColorTextDim, 148, 3);
    g_ui.ageLabel = makeLabel(g_ui.statusStrip, "--", &lv_font_montserrat_14, kColorText, 218, 1);

    // ---- footer: an explicit way to change page.
    // Swiping is the intended gesture, but nothing on screen said so, and a swipe
    // that starts on a card is easy to get wrong. These buttons make the action
    // discoverable and give a reliable fallback on a resistive panel.
    g_ui.prevButton = makeNavButton(g_dashboard, LV_SYMBOL_LEFT, kLeftX, kFooterY, 44);
    lv_obj_add_event_cb(g_ui.prevButton, onPrevPage, LV_EVENT_CLICKED, nullptr);

    // Real dots rather than "#  -  -" in a text label: the ASCII version was the
    // one part of the footer that looked like a placeholder.
    for (uint8_t i = 0; i < kMaxDots; ++i) {
        lv_obj_t* dot = lv_obj_create(g_dashboard);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, kColorBorder, LV_PART_MAIN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        g_ui.dots[i] = dot;
    }

    g_ui.nextButton = makeNavButton(g_dashboard, LV_SYMBOL_RIGHT, 216, kFooterY, 44);
    lv_obj_add_event_cb(g_ui.nextButton, onNextPage, LV_EVENT_CLICKED, nullptr);

    g_ui.gear = makeNavButton(g_dashboard, LV_SYMBOL_SETTINGS, 264, kFooterY, 52);
    lv_obj_add_event_cb(g_ui.gear, onGearClicked, LV_EVENT_CLICKED, nullptr);

    // Shown instead of the tiles when no device has been added yet.
    g_ui.emptyLabel = makeLabel(g_dashboard, "", &lv_font_montserrat_14, kColorTextDim, 16, 90);
    lv_obj_set_width(g_ui.emptyLabel, 288);
    lv_label_set_long_mode(g_ui.emptyLabel, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(g_ui.emptyLabel, LV_OBJ_FLAG_HIDDEN);
}

void setTilesHidden(bool hidden) {
    lv_obj_t* tiles[] = {g_ui.cpuCard, g_ui.ramCard,     g_ui.diskCard,
                         g_ui.netCard, g_ui.statusStrip, g_ui.prevButton,
                         g_ui.nextButton};
    for (lv_obj_t* tile : tiles) {
        if (tile == nullptr) {
            continue;
        }
        if (hidden) {
            lv_obj_add_flag(tile, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void renderEmptyState() {
    setTilesHidden(true);
    lv_obj_clear_flag(g_ui.emptyLabel, LV_OBJ_FLAG_HIDDEN);

    app::AppState& state = app::state();
    app::AppState::Lock lock(state);
    const app::RuntimeStatus& status = state.status();
    char text[256];
    if (status.wifi == app::WifiPhase::Portal) {
        snprintf(text, sizeof(text),
                 "Wi-Fi setup needed.\n\nJoin the network\n  %s\nthen open\n  http://%s/\n\n"
                 "Or tap the gear to set up Wi-Fi here.",
                 status.apSsid, status.apIp);
    } else if (status.wifi != app::WifiPhase::Connected) {
        snprintf(text, sizeof(text), "Connecting to Wi-Fi...\n%s", net::wifi().lastMessage());
    } else if (!state.devices().pending().empty()) {
        snprintf(text, sizeof(text),
                 "%u device(s) discovered and waiting for approval.\n\n"
                 "Tap the gear then Devices to add them.",
                 (unsigned)state.devices().pending().size());
    } else {
        snprintf(text, sizeof(text),
                 "No devices yet.\n\nInstall fleetpanel-agent on a machine, or add one at\n"
                 "http://%s/",
                 status.ip[0] ? status.ip : "wikistats.local");
    }
    lv_label_set_text(g_ui.emptyLabel, text);
    lv_label_set_text(g_ui.nameLabel, "WikiStats");
    lv_label_set_text(g_ui.pageLabel, "0/0");
    renderDots(0, -1);
}

void renderPage(const PageSnapshot& page) {
    setTilesHidden(false);
    lv_obj_add_flag(g_ui.emptyLabel, LV_OBJ_FLAG_HIDDEN);

    const fp::Telemetry& t = page.telemetry;
    const fp::Thresholds& th = page.thresholds;

    lv_label_set_text(g_ui.nameLabel, page.label.c_str());
    lv_label_set_text(g_ui.platformLabel, platformSymbol(page.platform));
    lv_label_set_text(g_ui.stateLabel, fp::freshnessName(page.freshness));
    lv_obj_set_style_text_color(g_ui.stateLabel, colourFor(page.freshness), LV_PART_MAIN);
    lv_label_set_text_fmt(g_ui.pageLabel, "%d/%d", page.pageIndex + 1, page.pageCount);

    {
        app::AppState& state = app::state();
        app::AppState::Lock lock(state);
        const app::RuntimeStatus& status = state.status();
        lv_label_set_text(g_ui.wifiLabel,
                          wifiSymbolFor(status.rssi, status.wifi == app::WifiPhase::Connected));
        lv_obj_set_style_text_color(
            g_ui.wifiLabel,
            status.wifi == app::WifiPhase::Connected ? kColorAccent : kColorCritical, LV_PART_MAIN);
    }

    // ---- CPU
    const fp::Level cpuLevel =
        fp::levelForOptional(t.cpuPercent.has, t.cpuPercent.value, th.cpuWarn, th.cpuCritical);
    lv_label_set_text(g_ui.cpuValue,
                      t.cpuPercent.has ? fp::formatPercentWhole(t.cpuPercent.value).c_str()
                                       : fp::kNoValue);
    applyLevel(g_ui.cpuValue, g_ui.cpuTag, cpuLevel);

    const fp::Level tempLevel = fp::levelForOptional(t.cpuTemperature.has, t.cpuTemperature.value,
                                                     th.cpuTempWarn, th.cpuTempCritical);
    lv_label_set_text(g_ui.cpuTemp,
                      t.cpuTemperature.has ? fp::formatTemperature(t.cpuTemperature.value).c_str()
                                           : fp::kNoValue);
    applyLevel(g_ui.cpuTemp, g_ui.cpuTempTag, tempLevel);
    setChartFrom(g_ui.cpuChart, g_ui.cpuSeries, page.cpuHistory, page.cpuCount);

    // ---- RAM
    const fp::Level ramLevel =
        fp::levelForOptional(t.memPercent.has, t.memPercent.value, th.ramWarn, th.ramCritical);
    lv_label_set_text(g_ui.ramValue,
                      t.memPercent.has ? fp::formatPercentWhole(t.memPercent.value).c_str()
                                       : fp::kNoValue);
    applyLevel(g_ui.ramValue, g_ui.ramTag, ramLevel);
    lv_bar_set_value(g_ui.ramBar, t.memPercent.has ? static_cast<int32_t>(t.memPercent.value) : 0,
                     LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_ui.ramBar, colourFor(ramLevel == fp::Level::Ok ? fp::Level::Unknown
                                                                              : ramLevel),
                              LV_PART_INDICATOR);
    if (ramLevel == fp::Level::Ok) {
        lv_obj_set_style_bg_color(g_ui.ramBar, kColorAccent, LV_PART_INDICATOR);
    }
    lv_label_set_text(g_ui.ramDetail,
                      (t.memUsed.has && t.memTotal.has)
                          ? fp::formatSharedUnit(static_cast<double>(t.memUsed.value),
                                                 static_cast<double>(t.memTotal.value))
                                .c_str()
                          : fp::kNoValue);
    setChartFrom(g_ui.ramChart, g_ui.ramSeries, page.ramHistory, page.ramCount);

    // ---- storage
    const fp::Level diskLevel =
        fp::levelForOptional(t.diskPercent.has, t.diskPercent.value, th.diskWarn, th.diskCritical);
    lv_label_set_text(g_ui.diskValue,
                      t.diskPercent.has ? fp::formatPercentWhole(t.diskPercent.value).c_str()
                                        : fp::kNoValue);
    applyLevel(g_ui.diskValue, g_ui.diskTag, diskLevel);
    lv_bar_set_value(g_ui.diskBar,
                     t.diskPercent.has ? static_cast<int32_t>(t.diskPercent.value) : 0,
                     LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_ui.diskBar,
                              diskLevel == fp::Level::Ok ? kColorAccent : colourFor(diskLevel),
                              LV_PART_INDICATOR);
    // "27.7 of 62.5 GiB" reads shorter than "27.7 GiB used of 62.5 GiB" and, because
    // both numbers share the total's scale, they line up column-wise as the values
    // change.
    lv_label_set_text(g_ui.diskDetail,
                      (t.diskUsed.has && t.diskTotal.has)
                          ? fp::formatSharedUnit(static_cast<double>(t.diskUsed.value),
                                                 static_cast<double>(t.diskTotal.value))
                                .c_str()
                          : fp::kNoValue);
    if (t.diskFree.has) {
        char freeText[40];
        snprintf(freeText, sizeof(freeText), "%s free",
                 fp::formatBytes(static_cast<double>(t.diskFree.value)).c_str());
        lv_label_set_text(g_ui.diskFree, freeText);
    } else {
        lv_label_set_text(g_ui.diskFree, fp::kNoValue);
    }

    // ---- network
    lv_label_set_text(g_ui.rxLabel,
                      t.rxRate.has ? fp::formatRate(t.rxRate.value).c_str() : fp::kNoValue);
    lv_label_set_text(g_ui.txLabel,
                      t.txRate.has ? fp::formatRate(t.txRate.value).c_str() : fp::kNoValue);

    // ---- status strip
    lv_label_set_text(g_ui.uptimeLabel,
                      t.uptimeSeconds.has ? fp::formatUptime(t.uptimeSeconds.value).c_str()
                                          : fp::kNoValue);
    lv_label_set_text(g_ui.ageLabel, fp::formatAge(page.ageSeconds).c_str());
    lv_obj_set_style_text_color(g_ui.ageLabel, colourFor(page.freshness), LV_PART_MAIN);

    renderDots(page.pageCount, page.pageIndex);
}

// Centres up to kMaxDots indicators in the footer strip. Beyond that the header's
// "4/17" counter is the honest signal and drawing seventeen dots would be noise.
void renderDots(int count, int active) {
    const int shown = count > kMaxDots ? kMaxDots : count;
    const lv_coord_t spacing = 14;
    const lv_coord_t totalWidth = shown > 0 ? (shown - 1) * spacing + 8 : 0;
    const lv_coord_t startX = kDotsRect.x + (kDotsRect.w - totalWidth) / 2;
    const lv_coord_t y = kFooterY + (kFooterHeight - 8) / 2;

    for (int i = 0; i < kMaxDots; ++i) {
        lv_obj_t* dot = g_ui.dots[i];
        if (dot == nullptr) {
            continue;
        }
        if (i >= shown) {
            lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(dot, startX + i * spacing, y);
        lv_obj_set_style_bg_color(dot, i == active ? kColorAccent : kColorBorder, LV_PART_MAIN);
    }
}

void render() {
    PageSnapshot page;
    if (!snapshotPage(page)) {
        renderEmptyState();
        return;
    }
    renderPage(page);
}

// ---------------------------------------------------------- navigation

void changePage(int delta, bool animate) {
    const int count = visibleCount();
    if (count <= 1) {
        return;
    }
    int next = g_pageIndex + delta;
    if (next < 0) {
        next = count - 1;
    } else if (next >= count) {
        next = 0;
    }
    if (next == g_pageIndex) {
        return;
    }
    g_pageIndex = next;
    g_carousel.notePageChange(millis());
    render();
    if (animate) {
        // A short slide gives the swipe a direction without costing frames: the
        // dashboard is one screen, so this animates the whole thing at once.
        lv_obj_set_style_opa(g_dashboard, LV_OPA_COVER, LV_PART_MAIN);
    }
    LOG_D(kTag, "page %d/%d", g_pageIndex + 1, count);
}

void advanceCarousel(uint32_t nowMs) {
    app::AppState& state = app::state();
    std::vector<bool> eligible;
    int current = g_pageIndex;
    {
        app::AppState::Lock lock(state);
        g_carousel.setSettings(state.config().carousel);
        const std::vector<size_t> order = state.devices().visibleOrder();
        const fp::Thresholds& thresholds = state.config().thresholds;
        const bool includeOffline = state.config().carousel.includeOffline;
        eligible.reserve(order.size());
        for (const size_t index : order) {
            const fp::DeviceState& device = state.devices().all()[index];
            bool ok = device.config.carousel;
            if (ok && !includeOffline) {
                const fp::Freshness freshness =
                    device.announcedOffline
                        ? fp::Freshness::Offline
                        : fp::freshnessFor(device.ageSeconds(nowMs), device.everReceived,
                                           thresholds);
                ok = freshness == fp::Freshness::Fresh || freshness == fp::Freshness::Stale;
            }
            eligible.push_back(ok);
        }
    }
    if (!g_carousel.shouldAdvance(nowMs)) {
        return;
    }
    const int next = g_carousel.nextIndex(current, eligible);
    if (next < 0) {
        g_carousel.notePageChange(nowMs);  // nowhere to go; restart the dwell timer
        return;
    }
    g_pageIndex = next;
    g_carousel.notePageChange(nowMs);
    render();
}

// ------------------------------------------------------------ backlight

void applyBacklight(uint32_t nowMs) {
    app::AppState& state = app::state();
    uint8_t bright = 80;
    uint8_t dim = 10;
    uint32_t timeout = 0;
    {
        app::AppState::Lock lock(state);
        bright = state.config().display.brightness;
        dim = state.config().display.dimBrightness;
        timeout = state.config().display.screenTimeoutSeconds;
    }
    const bool shouldDim = timeout > 0 && (nowMs - g_lastInteractionMs) > timeout * 1000u;
    if (shouldDim != g_dimmed) {
        g_dimmed = shouldDim;
        hal::display().setBacklight(shouldDim ? dim : bright);
    } else if (!shouldDim && hal::display().backlight() != bright) {
        hal::display().setBacklight(bright);
    }
}

// -------------------------------------------------------------- gestures

// Dispatches a tap the gesture detector has already accepted. Because the detector
// rejects anything that moved beyond the slop threshold, a swipe can never reach
// this - which is the whole point of not letting LVGL own these hit boxes.
void dispatchTap(int16_t x, int16_t y) {
    if (kHeaderRect.contains(x, y)) {
        showDeviceInfo(nullptr);
    } else if (kCpuRect.contains(x, y)) {
        showCpuDetail(nullptr);
    } else if (kRamRect.contains(x, y)) {
        showMemoryDetail(nullptr);
    } else if (kDiskRect.contains(x, y)) {
        showStorageDetail(nullptr);
    } else if (kNetRect.contains(x, y)) {
        showNetworkDetail(nullptr);
    }
    // The footer belongs to real LVGL buttons; nothing to do here.
}

void processTouch(uint32_t nowMs) {
    const bool wasPressed = g_gestures.active();
    hal::TouchPoint point;
    const bool pressed = hal::display().readTouch(point);
    g_touch = pressed ? point : hal::TouchPoint{};

    if (pressed && !wasPressed) {
        g_gestures.press(point.x, point.y, nowMs);
        noteInteraction();
    } else if (pressed && wasPressed) {
        g_gestures.move(point.x, point.y, nowMs);
        g_lastInteractionMs = nowMs;
        // Only arm the long press over the page indicator. Polling it everywhere
        // latched `longPressFired_` on any slow press, and a press that has fired a
        // long press deliberately produces no tap on release - so a leisurely tap
        // on a card did nothing at all.
        const bool overIndicator = kDotsRect.contains(g_gestures.startX(), g_gestures.startY());
        if (overIndicator && lv_scr_act() == g_dashboard && g_gestures.pollLongPress(nowMs)) {
            showDeviceList();
        }
    } else if (!pressed && wasPressed) {
        const int16_t releaseX = g_gestures.startX();
        const int16_t releaseY = g_gestures.startY();
        const fp::Gesture gesture = g_gestures.release(g_touch.x, g_touch.y, nowMs);
        noteInteraction();
        if (lv_scr_act() != g_dashboard) {
            return;  // other screens use LVGL's own widgets, not page swipes
        }
        switch (gesture) {
            case fp::Gesture::SwipeLeft:
                changePage(+1, true);
                break;
            case fp::Gesture::SwipeRight:
                changePage(-1, true);
                break;
            case fp::Gesture::Tap:
                // Hit-tested from where the finger went down, so a tap that drifts
                // a few pixels across a card boundary still opens what was aimed at.
                dispatchTap(releaseX, releaseY);
                break;
            default:
                break;
        }
    }
}

}  // namespace

// ------------------------------------------------------------- public

lv_obj_t* dashboardScreen() { return g_dashboard; }

void noteInteraction() {
    g_lastInteractionMs = millis();
    g_carousel.noteInteraction(g_lastInteractionMs);
    if (g_dimmed) {
        g_dimmed = false;
        app::AppState& state = app::state();
        app::AppState::Lock lock(state);
        hal::display().setBacklight(state.config().display.brightness);
    }
}

uint32_t lastInteractionMs() { return g_lastInteractionMs; }

void setPageIndex(int index) {
    g_pageIndex = index;
    g_carousel.notePageChange(millis());
    if (lv_scr_act() == g_dashboard) {
        render();
    }
}

int pageIndex() { return g_pageIndex; }

void showDashboard() {
    if (g_dashboard == nullptr) {
        return;
    }
    render();
    lv_scr_load_anim(g_dashboard, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, false);
}

void toast(const char* text) {
    if (g_toast != nullptr) {
        lv_obj_del(g_toast);
        g_toast = nullptr;
    }
    g_toast = lv_label_create(lv_layer_top());
    lv_label_set_text(g_toast, text);
    lv_obj_set_style_bg_color(g_toast, kColorCardAlt, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_toast, kColorText, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_toast, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(g_toast, 6, LV_PART_MAIN);
    lv_obj_align(g_toast, LV_ALIGN_BOTTOM_MID, 0, -40);
    // Exactly one toast object exists at a time: the previous one is deleted above,
    // so a device that runs for months does not accumulate hidden labels.
    lv_obj_fade_out(g_toast, 400, 2000);
}

void showBootMessage(const char* title, const char* detail) {
    lv_obj_t* screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, kColorBackground, LV_PART_MAIN);
    makeLabel(screen, title, &lv_font_montserrat_20, kColorText, 16, 60);
    lv_obj_t* body = makeLabel(screen, detail, &lv_font_montserrat_14, kColorTextDim, 16, 96);
    lv_obj_set_width(body, 288);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_timer_handler();
}

bool begin() {
    lv_init();

    const uint32_t pixels = kScreenW * kDrawBufferLines;
    g_pixels = static_cast<lv_color_t*>(
        heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (g_pixels == nullptr) {
        LOG_E(kTag, "cannot allocate the %u byte draw buffer",
              (unsigned)(pixels * sizeof(lv_color_t)));
        return false;
    }
    lv_disp_draw_buf_init(&g_drawBuffer, g_pixels, nullptr, pixels);

    lv_disp_drv_init(&g_dispDriver);
    g_dispDriver.hor_res = kScreenW;
    g_dispDriver.ver_res = kScreenH;
    g_dispDriver.flush_cb = flushCallback;
    g_dispDriver.draw_buf = &g_drawBuffer;
    lv_disp_drv_register(&g_dispDriver);

    lv_indev_drv_init(&g_indevDriver);
    g_indevDriver.type = LV_INDEV_TYPE_POINTER;
    g_indevDriver.read_cb = touchCallback;
    g_indev = lv_indev_drv_register(&g_indevDriver);

    initTheme();
    buildDashboard();
    buildSettingsScreen();
    buildWifiScreen();
    buildDeviceListScreen();
    buildDiagnosticsScreen();

    {
        app::AppState& state = app::state();
        app::AppState::Lock lock(state);
        g_carousel.setSettings(state.config().carousel);
        hal::display().setBacklight(state.config().display.brightness);
    }
    g_lastInteractionMs = millis();
    lv_scr_load(g_dashboard);
    render();
    LOG_I(kTag, "UI ready (%u byte draw buffer, free heap %u)",
          (unsigned)(pixels * sizeof(lv_color_t)), (unsigned)ESP.getFreeHeap());
    return true;
}

void tick() {
    const uint32_t now = millis();
    processTouch(now);

    const uint32_t revision = app::state().revision();
    lv_obj_t* active = lv_scr_act();
    if (revision != g_lastRevision || (now - g_lastRenderMs) > 1000) {
        g_lastRevision = revision;
        g_lastRenderMs = now;
        if (active == g_dashboard) {
            render();
        } else if (active == settingsScreen()) {
            refreshSettingsScreen();
        } else if (active == wifiScreen()) {
            refreshWifiScreen();
        } else if (active == deviceListScreen()) {
            refreshDeviceListScreen();
        } else if (active == diagnosticsScreen()) {
            refreshDiagnosticsScreen();
        }
    }

    if (lv_scr_act() == g_dashboard) {
        advanceCarousel(now);
    }
    applyBacklight(now);
    // Arms a pending frame capture before the redraw, so the flush callback can
    // stream the bands out as they are produced.
    shot::serviceUi();
    lv_timer_handler();
}

}  // namespace ui
