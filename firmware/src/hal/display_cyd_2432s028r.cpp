// ESP32-2432S028R "Cheap Yellow Display" implementation of DisplayHal.
//
//   Panel  ILI9341 320x240, HSPI (MOSI 13, MISO 12, SCLK 14, CS 15, DC 2, BL 21)
//   Touch  XPT2046 on a *separate* VSPI bus (CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36)
//   Extras RGB LED on 4/16/17 (active low), LDR on 34
//
// The two SPI buses are why TFT_eSPI's built-in touch support is not used: it
// assumes the touch controller shares the display bus. Driving XPT2046 on its own
// bus also lets it run at 2.5 MHz while the panel runs at 55 MHz.
//
// All pin numbers come from platformio.ini, so a variant with a different map is a
// build-flag change rather than an edit here.

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include "display_hal.h"

namespace hal {

namespace {

// Raw XPT2046 corners. These are the values the great majority of 2432S028R boards
// produce; a panel that reads noticeably off can be corrected with -D overrides
// without touching this file.
#ifndef FP_TOUCH_RAW_MIN_X
#define FP_TOUCH_RAW_MIN_X 200
#endif
#ifndef FP_TOUCH_RAW_MAX_X
#define FP_TOUCH_RAW_MAX_X 3700
#endif
#ifndef FP_TOUCH_RAW_MIN_Y
#define FP_TOUCH_RAW_MIN_Y 240
#endif
#ifndef FP_TOUCH_RAW_MAX_Y
#define FP_TOUCH_RAW_MAX_Y 3800
#endif

// Below this the reading is noise, not a finger. Resistive panels report a small
// non-zero pressure from mechanical stress alone.
constexpr uint16_t kMinPressure = 200;

constexpr uint16_t kScreenWidth = 320;
constexpr uint16_t kScreenHeight = 240;

constexpr uint8_t kBacklightPwmChannel = FP_BACKLIGHT_PWM_CHANNEL;
constexpr uint32_t kBacklightPwmFreq = 5000;
constexpr uint8_t kBacklightPwmBits = 8;

class CydDisplay final : public DisplayHal {
   public:
    bool begin(uint8_t rotation) override {
        tft_.init();
        tft_.setRotation(rotation);
        tft_.fillScreen(TFT_BLACK);

        // Touch gets its own bus instance so its clock and chip select are fully
        // independent of the panel's.
        touchSpi_.begin(FP_TOUCH_CLK, FP_TOUCH_MISO, FP_TOUCH_MOSI, FP_TOUCH_CS);
        touch_.begin(touchSpi_);
        touch_.setRotation(rotation);

        pinMode(FP_LED_R, OUTPUT);
        pinMode(FP_LED_G, OUTPUT);
        pinMode(FP_LED_B, OUTPUT);
        setStatusLed(0, 0, 0);

        pinMode(FP_LDR_PIN, INPUT);
        analogReadResolution(12);

        ledcSetup(kBacklightPwmChannel, kBacklightPwmFreq, kBacklightPwmBits);
        ledcAttachPin(FP_BACKLIGHT_PIN, kBacklightPwmChannel);
        setBacklight(80);

        started_ = true;
        return true;
    }

    void flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t* pixels) override {
        const uint32_t width = static_cast<uint32_t>(x2 - x1 + 1);
        const uint32_t height = static_cast<uint32_t>(y2 - y1 + 1);
        tft_.startWrite();
        tft_.setAddrWindow(x1, y1, width, height);
        // `true` swaps bytes here rather than in LVGL (LV_COLOR_16_SWAP 0); doing it
        // in both places would cancel out and give an inverted palette.
        tft_.pushColors(pixels, width * height, true);
        tft_.endWrite();
    }

    bool readTouch(TouchPoint& out) override {
        out = TouchPoint{};
        if (!started_ || !touch_.tirqTouched() || !touch_.touched()) {
            return false;
        }
        const TS_Point point = touch_.getPoint();
        if (point.z < kMinPressure) {
            return false;
        }
        out.pressed = true;
        out.pressure = static_cast<uint16_t>(point.z);
        out.x = static_cast<int16_t>(
            clamp(map(point.x, FP_TOUCH_RAW_MIN_X, FP_TOUCH_RAW_MAX_X, 0, kScreenWidth), 0,
                  kScreenWidth - 1));
        out.y = static_cast<int16_t>(
            clamp(map(point.y, FP_TOUCH_RAW_MIN_Y, FP_TOUCH_RAW_MAX_Y, 0, kScreenHeight), 0,
                  kScreenHeight - 1));
        return true;
    }

    void setBacklight(uint8_t percent) override {
        backlight_ = percent > 100 ? 100 : percent;
        // Perceptual rather than linear: the bottom of a linear PWM ramp is all the
        // visible change, so 20% would look nearly as bright as 60%.
        const uint32_t duty = (static_cast<uint32_t>(backlight_) * backlight_ * 255u) / 10000u;
        ledcWrite(kBacklightPwmChannel, duty);
    }

    uint8_t backlight() const override { return backlight_; }

    int16_t ambientLight() const override {
        const int raw = analogRead(FP_LDR_PIN);
        if (raw < 0) {
            return -1;
        }
        // The CYD wires the LDR as a pull-down, so a bright room reads *low*.
        const int inverted = 4095 - raw;
        return static_cast<int16_t>(clamp((inverted * 100) / 4095, 0, 100));
    }

    void setStatusLed(uint8_t red, uint8_t green, uint8_t blue) override {
        // The RGB LED is common-anode: LOW lights the channel.
        digitalWrite(FP_LED_R, red > 127 ? LOW : HIGH);
        digitalWrite(FP_LED_G, green > 127 ? LOW : HIGH);
        digitalWrite(FP_LED_B, blue > 127 ? LOW : HIGH);
    }

    const DisplayInfo& info() const override { return info_; }

   private:
    static long clamp(long value, long low, long high) {
        return value < low ? low : (value > high ? high : value);
    }

    TFT_eSPI tft_{kScreenWidth, kScreenHeight};
    SPIClass touchSpi_{FP_TOUCH_SPI_HOST};
    XPT2046_Touchscreen touch_{FP_TOUCH_CS, FP_TOUCH_IRQ};
    uint8_t backlight_ = 80;
    bool started_ = false;
    DisplayInfo info_{"ESP32-2432S028R (Cheap Yellow Display)",
                      "ILI9341",
                      "XPT2046",
                      kScreenWidth,
                      kScreenHeight,
                      true};
};

CydDisplay g_display;

}  // namespace

DisplayHal& display() { return g_display; }

}  // namespace hal
