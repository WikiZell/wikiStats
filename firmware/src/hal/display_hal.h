// Hardware abstraction for the panel.
//
// Everything above this line talks to `hal::display()`; nothing above it includes
// TFT_eSPI or the touch driver. Supporting another Cheap Yellow Display variant -
// the ST7789 2432S028Rv3, the 3.5" 3248S035, a capacitive board - means adding one
// .cpp that implements this interface plus a pin block in platformio.ini, with no
// change to the UI, the transports or the configuration model.
#pragma once

#include <cstdint>

namespace hal {

struct TouchPoint {
    bool pressed = false;
    int16_t x = 0;
    int16_t y = 0;
    uint16_t pressure = 0;
};

struct DisplayInfo {
    const char* boardName;
    const char* panelDriver;
    const char* touchDriver;
    uint16_t width;
    uint16_t height;
    bool hasBacklightPwm;
};

class DisplayHal {
   public:
    virtual ~DisplayHal() = default;

    // `rotation` follows the TFT_eSPI convention: 1 = landscape with the USB socket
    // on the left, which is the orientation the UI is laid out for.
    virtual bool begin(uint8_t rotation) = 0;

    // Push a rectangle of RGB565 pixels. Called from the LVGL flush callback only.
    virtual void flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t* pixels) = 0;

    // Returns false when the panel is not being touched.
    virtual bool readTouch(TouchPoint& out) = 0;

    // 0-100. 0 turns the backlight off without powering down the controller, so the
    // screen wakes instantly on touch.
    virtual void setBacklight(uint8_t percent) = 0;
    virtual uint8_t backlight() const = 0;

    // Ambient light from the on-board LDR, 0-100, or -1 when the board has none.
    virtual int16_t ambientLight() const = 0;

    // The RGB LED behind the CYD's front panel, used for critical-state signalling.
    virtual void setStatusLed(uint8_t red, uint8_t green, uint8_t blue) = 0;

    virtual const DisplayInfo& info() const = 0;
};

// The board compiled into this build.
DisplayHal& display();

}  // namespace hal
