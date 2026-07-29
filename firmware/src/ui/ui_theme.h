// Visual system for a 320x240 panel.
//
// Deliberate constraints:
//   * One accent colour. Warning and critical are the only other hues, so when
//     something turns amber or red it means something.
//   * Four type sizes. A dashboard that needs five is a cluttered dashboard.
//   * Touch targets never below 40 px on their short edge - roughly a fingertip.
//   * State is never signalled by colour alone; every warning also carries a text
//     tag, because a red tile is invisible to a red-green colour blind viewer and
//     unreadable through a glossy resistive overlay at an angle.
#pragma once

#include <lvgl.h>

#include "fp_thresholds.h"

namespace ui {

// ---- palette (dark, low chroma background so the accent actually stands out)
extern const lv_color_t kColorBackground;
extern const lv_color_t kColorCard;
extern const lv_color_t kColorCardAlt;
extern const lv_color_t kColorBorder;
extern const lv_color_t kColorText;
extern const lv_color_t kColorTextDim;
extern const lv_color_t kColorAccent;
extern const lv_color_t kColorWarning;
extern const lv_color_t kColorCritical;
extern const lv_color_t kColorOffline;

// ---- geometry
constexpr lv_coord_t kScreenW = 320;
constexpr lv_coord_t kScreenH = 240;
constexpr lv_coord_t kGutter = 4;
constexpr lv_coord_t kHeaderH = 26;
constexpr lv_coord_t kFooterH = 28;
constexpr lv_coord_t kTouchTargetMin = 40;

void initTheme();

// Card container with the standard padding, radius and border.
lv_obj_t* makeCard(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);
lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t colour,
                    lv_coord_t x, lv_coord_t y);
lv_obj_t* makeButton(lv_obj_t* parent, const char* text, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                     lv_coord_t h);
lv_obj_t* makeBar(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);
lv_obj_t* makeSparkline(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                        lv_chart_series_t** seriesOut);

lv_color_t colourFor(fp::Level level);
lv_color_t colourFor(fp::Freshness freshness);

// Applies the level colour to a value label *and* writes the text tag next to it.
void applyLevel(lv_obj_t* valueLabel, lv_obj_t* tagLabel, fp::Level level);

}  // namespace ui
