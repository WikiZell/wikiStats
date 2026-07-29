#include "fp_gesture.h"

#include <cmath>
#include <cstdlib>

namespace fp {

const char* gestureName(Gesture gesture) {
    switch (gesture) {
        case Gesture::Tap:
            return "tap";
        case Gesture::LongPress:
            return "long-press";
        case Gesture::SwipeLeft:
            return "swipe-left";
        case Gesture::SwipeRight:
            return "swipe-right";
        case Gesture::None:
        default:
            return "none";
    }
}

void GestureDetector::press(int16_t x, int16_t y, uint32_t timeMs) {
    active_ = true;
    longPressFired_ = false;
    exceededSlop_ = false;
    startX_ = lastX_ = x;
    startY_ = lastY_ = y;
    startMs_ = timeMs;
}

void GestureDetector::move(int16_t x, int16_t y, uint32_t timeMs) {
    (void)timeMs;
    if (!active_) {
        return;
    }
    lastX_ = x;
    lastY_ = y;
    if (std::abs(static_cast<int>(x) - startX_) > config_.tapSlop ||
        std::abs(static_cast<int>(y) - startY_) > config_.tapSlop) {
        exceededSlop_ = true;
    }
}

bool GestureDetector::pollLongPress(uint32_t timeMs) {
    if (!active_ || longPressFired_ || exceededSlop_) {
        return false;
    }
    if (timeMs - startMs_ >= config_.longPressMs) {
        longPressFired_ = true;
        return true;
    }
    return false;
}

Gesture GestureDetector::release(int16_t x, int16_t y, uint32_t timeMs) {
    if (!active_) {
        return Gesture::None;
    }
    active_ = false;
    lastX_ = x;
    lastY_ = y;

    const int32_t dx = static_cast<int32_t>(x) - startX_;
    const int32_t dy = static_cast<int32_t>(y) - startY_;
    const int32_t adx = std::abs(dx);
    const int32_t ady = std::abs(dy);
    const uint32_t duration = timeMs - startMs_;

    if (longPressFired_) {
        // The long press was already reported while the finger was down; releasing
        // must not also produce a tap.
        return Gesture::None;
    }

    const bool farEnough = adx >= config_.minSwipeDistance;
    const bool horizontal = static_cast<float>(ady) <= config_.maxVerticalRatio * static_cast<float>(adx);
    const bool inTime = duration <= config_.maxSwipeDurationMs && duration >= config_.minSwipeDurationMs;

    if (farEnough && horizontal && inTime) {
        return dx < 0 ? Gesture::SwipeLeft : Gesture::SwipeRight;
    }

    if (!exceededSlop_ && duration <= config_.maxTapDurationMs) {
        return Gesture::Tap;
    }

    // Deliberately nothing: a slow drag, a mostly-vertical flick, or a short
    // horizontal wobble are all ambiguous, and guessing would change the page under
    // the user's finger.
    return Gesture::None;
}

void GestureDetector::reset() {
    active_ = false;
    longPressFired_ = false;
    exceededSlop_ = false;
}

}  // namespace fp
