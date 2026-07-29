/**
 * @file lv_conf.h
 * LVGL 8.3 configuration for the WikiStats panel (ESP32-WROOM-32, no PSRAM).
 *
 * Only the values that differ from LVGL's defaults are listed. `lv_conf_internal.h`
 * supplies a default for every macro this file does not define, so keeping the file
 * short makes the deliberate choices obvious instead of burying them in 900 lines of
 * boilerplate.
 *
 * Reached via `-D LV_CONF_INCLUDE_SIMPLE -I include` in platformio.ini.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
 *   COLOR SETTINGS
 *====================*/

#define LV_COLOR_DEPTH 16

/* The byte swap ILI9341 needs is done by TFT_eSPI's pushColors(..., true) in the
 * flush callback, so LVGL itself must not swap. Setting both would cancel out and
 * produce a blue/orange inverted display. */
#define LV_COLOR_16_SWAP 0

/*=========================
 *   MEMORY SETTINGS
 *=========================*/

/* Use the system heap rather than a fixed LVGL pool. On a PSRAM-less ESP32 the
 * ~290 KiB of usable DRAM is shared with the Wi-Fi stack and the JSON parser, and a
 * static reservation large enough for the UI would starve them during a TLS
 * handshake. */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC malloc
#define LV_MEM_CUSTOM_FREE free
#define LV_MEM_CUSTOM_REALLOC realloc

/*====================
 *   HAL SETTINGS
 *====================*/

#define LV_DISP_DEF_REFR_PERIOD 20  /* ~50 Hz ceiling; the SPI bus is the real limit */
#define LV_INDEV_DEF_READ_PERIOD 20

/* Take the tick from Arduino's millis() so no timer ISR or lv_tick_inc() call is
 * needed, and a busy network task cannot make LVGL's animations run slow. */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF 130

/*=======================
 *  FEATURE CONFIGURATION
 *=======================*/

#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_IMG_CACHE_DEF_SIZE 0
#define LV_DISP_ROT_MAX_BUF (10U * 1024U)

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

/*==================
 *   FONT USAGE
 *===================*/

/* Four sizes only. Every enabled font costs flash, and a 320x240 panel that needs
 * five type sizes is a cluttered panel. */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
/* montserrat_28 was dropped: it cost ~14 KiB of flash for one number, and 20 pt is
 * already the largest size that leaves room for a unit suffix on a 152 px card. */

/*=================
 *  TEXT SETTINGS
 *=================*/

#define LV_TXT_ENC LV_TXT_ENC_UTF8

/*===================
 *  WIDGET USAGE
 *==================*/

#define LV_USE_ARC 0
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_BTNMATRIX 1
#define LV_USE_CANVAS 0
#define LV_USE_CHECKBOX 1
#define LV_USE_DROPDOWN 1
#define LV_USE_IMG 1
#define LV_USE_LABEL 1
#define LV_LABEL_TEXT_SELECTION 0
#define LV_LABEL_LONG_TXT_HINT 1
#define LV_USE_LINE 1
#define LV_USE_ROLLER 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TEXTAREA 1
#define LV_USE_TABLE 0

#define LV_USE_CHART 1      /* CPU and RAM sparklines */
#define LV_USE_KEYBOARD 1   /* Wi-Fi password entry on the touchscreen */
#define LV_USE_LIST 1       /* network scan results, device list */
#define LV_USE_MSGBOX 1     /* metric detail dialogs, confirmations */
#define LV_USE_SPINNER 0    /* needs LV_USE_ARC; progress is reported as text */
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_CALENDAR 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN 0
#define LV_USE_LED 0
#define LV_USE_METER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_MENU 0
#define LV_USE_ANIMIMG 0

/*==================
 * THEME
 *==================*/

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 0            /* no scale-on-press: it costs redraws */
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO 0

/*==================
 * LAYOUTS
 *==================*/

#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*==================
 * OTHERS
 *==================*/

#define LV_USE_SNAPSHOT 0
#define LV_USE_MONKEY 0
#define LV_USE_GRIDNAV 0
#define LV_USE_FRAGMENT 0
#define LV_USE_IMGFONT 0
#define LV_USE_MSG 0
#define LV_USE_QRCODE 0
#define LV_USE_FS_STDIO 0
#define LV_USE_PNG 0
#define LV_USE_BMP 0
#define LV_USE_SJPG 0
#define LV_USE_GIF 0
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0

#endif /* LV_CONF_H */
