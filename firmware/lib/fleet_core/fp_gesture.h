// Swipe / tap / long-press recognition for a resistive touchscreen.
//
// A resistive panel under a finger is noisy: contact bounces, the reported point
// drifts several pixels while pressure changes, and a "tap" can register two or
// three separate press events. The rules below exist to stop that noise from
// flipping the page:
//
//  * A swipe needs a minimum horizontal travel (default 45 px of a 320 px screen).
//  * The gesture must be mostly horizontal - vertical travel above a fraction of
//    the horizontal travel is rejected, so scrolling a list never changes device.
//  * A gesture slower than `maxDurationMs` is a drag, not a swipe.
//  * A press that never moves beyond `tapSlop` is a tap; held past
//    `longPressMs` it is a long press.
//
// No Arduino dependency: `pressed`/`moved`/`released` take an explicit millisecond
// timestamp, which is also what makes carousel-and-gesture interaction testable.
#pragma once

#include <cstdint>

namespace fp {

enum class Gesture : uint8_t {
    None = 0,
    Tap,
    LongPress,
    SwipeLeft,   // finger moved right-to-left
    SwipeRight,  // finger moved left-to-right
};

struct GestureConfig {
    int16_t minSwipeDistance = 45;
    float maxVerticalRatio = 0.7f;  // |dy| must stay below this fraction of |dx|
    uint32_t maxSwipeDurationMs = 1000;
    uint32_t minSwipeDurationMs = 30;
    int16_t tapSlop = 12;
    uint32_t longPressMs = 600;
    uint32_t maxTapDurationMs = 500;
};

const char* gestureName(Gesture gesture);

class GestureDetector {
   public:
    explicit GestureDetector(const GestureConfig& config = GestureConfig{}) : config_(config) {}

    void setConfig(const GestureConfig& config) { config_ = config; }
    const GestureConfig& config() const { return config_; }

    void press(int16_t x, int16_t y, uint32_t timeMs);
    void move(int16_t x, int16_t y, uint32_t timeMs);
    Gesture release(int16_t x, int16_t y, uint32_t timeMs);

    // Long press fires while the finger is still down, so the UI can react before
    // the user lifts. Returns true exactly once per press.
    bool pollLongPress(uint32_t timeMs);

    void reset();

    bool active() const { return active_; }
    int16_t startX() const { return startX_; }
    int16_t startY() const { return startY_; }

   private:
    GestureConfig config_;
    bool active_ = false;
    bool longPressFired_ = false;
    bool exceededSlop_ = false;
    int16_t startX_ = 0;
    int16_t startY_ = 0;
    int16_t lastX_ = 0;
    int16_t lastY_ = 0;
    uint32_t startMs_ = 0;
};

}  // namespace fp
