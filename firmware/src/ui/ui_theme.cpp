#include "ui_theme.h"

namespace ui {

const lv_color_t kColorBackground = LV_COLOR_MAKE(0x10, 0x13, 0x18);
const lv_color_t kColorCard = LV_COLOR_MAKE(0x1B, 0x20, 0x28);
const lv_color_t kColorCardAlt = LV_COLOR_MAKE(0x23, 0x29, 0x33);
const lv_color_t kColorBorder = LV_COLOR_MAKE(0x2E, 0x35, 0x40);
const lv_color_t kColorText = LV_COLOR_MAKE(0xE8, 0xEC, 0xF1);
const lv_color_t kColorTextDim = LV_COLOR_MAKE(0x8C, 0x97, 0xA6);
const lv_color_t kColorAccent = LV_COLOR_MAKE(0x3D, 0xA9, 0xFC);
const lv_color_t kColorWarning = LV_COLOR_MAKE(0xF5, 0xA6, 0x23);
const lv_color_t kColorCritical = LV_COLOR_MAKE(0xF2, 0x5C, 0x54);
const lv_color_t kColorOffline = LV_COLOR_MAKE(0x5A, 0x63, 0x70);

void initTheme() {
    lv_obj_t* screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, kColorBackground, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
}

lv_obj_t* makeCard(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, kColorCard, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, kColorBorder, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 4, LV_PART_MAIN);
    // No scrollbars on a card: they steal pixels and invite accidental drags that
    // would otherwise have been page swipes.
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    return card;
}

lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t colour,
                    lv_coord_t x, lv_coord_t y) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, colour, LV_PART_MAIN);
    lv_obj_set_pos(label, x, y);
    return label;
}

lv_obj_t* makeButton(lv_obj_t* parent, const char* text, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                     lv_coord_t h) {
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h < kTouchTargetMin ? kTouchTargetMin : h);
    lv_obj_set_style_bg_color(button, kColorCardAlt, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, kColorAccent, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, kColorText, LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

lv_obj_t* makeBar(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, kColorCardAlt, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, kColorAccent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    return bar;
}

lv_obj_t* makeSparkline(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                        lv_chart_series_t** seriesOut) {
    lv_obj_t* chart = lv_chart_create(parent);
    lv_obj_set_pos(chart, x, y);
    lv_obj_set_size(chart, w, h);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    // Points off: at this size they merge into a thick smear.
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    if (seriesOut != nullptr) {
        *seriesOut = lv_chart_add_series(chart, kColorAccent, LV_CHART_AXIS_PRIMARY_Y);
    }
    return chart;
}

lv_color_t colourFor(fp::Level level) {
    switch (level) {
        case fp::Level::Warning:
            return kColorWarning;
        case fp::Level::Critical:
            return kColorCritical;
        case fp::Level::Unknown:
            return kColorTextDim;
        case fp::Level::Ok:
        default:
            return kColorText;
    }
}

lv_color_t colourFor(fp::Freshness freshness) {
    switch (freshness) {
        case fp::Freshness::Fresh:
            return kColorAccent;
        case fp::Freshness::Stale:
            return kColorWarning;
        case fp::Freshness::Offline:
            return kColorCritical;
        case fp::Freshness::Never:
        default:
            return kColorOffline;
    }
}

void applyLevel(lv_obj_t* valueLabel, lv_obj_t* tagLabel, fp::Level level) {
    if (valueLabel != nullptr) {
        lv_obj_set_style_text_color(valueLabel, colourFor(level), LV_PART_MAIN);
    }
    if (tagLabel != nullptr) {
        // The tag is the non-colour signal. Empty for Ok and Unknown.
        lv_label_set_text(tagLabel, fp::levelTag(level));
        lv_obj_set_style_text_color(tagLabel, colourFor(level), LV_PART_MAIN);
    }
}

}  // namespace ui
