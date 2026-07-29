// Idle carousel timing.
//
// Pure time arithmetic driven by an injected millisecond clock, so the whole
// pause/resume/advance behaviour is unit tested without a display.
//
// Behaviour:
//   * Touch pauses immediately.
//   * The carousel resumes `idleResumeSeconds` after the *last* touch.
//   * Advancing skips devices that are not eligible (disabled, hidden, or - when
//     `includeOffline` is false - offline).
//   * With `wrap` disabled the carousel stops at the last device instead of
//     bouncing, which is what people expect from a signage-style rotation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fp {

struct CarouselSettings {
    bool enabled = true;
    uint32_t intervalSeconds = 10;      // clamped to [3, 120]
    uint32_t idleResumeSeconds = 30;    // 0 = resume immediately after touch ends
    bool wrap = true;
    bool includeOffline = true;

    void sanitise();
};

class Carousel {
   public:
    void setSettings(const CarouselSettings& settings);
    const CarouselSettings& settings() const { return settings_; }

    // Call on every touch event. Pauses rotation and restarts the idle timer.
    void noteInteraction(uint32_t nowMs);

    // Call when the displayed page changes for any reason, so the dwell timer
    // measures time on the current page rather than time since boot.
    void notePageChange(uint32_t nowMs);

    bool paused(uint32_t nowMs) const;
    uint32_t secondsUntilResume(uint32_t nowMs) const;

    // True once per interval while running. The caller then asks for nextIndex().
    bool shouldAdvance(uint32_t nowMs) const;

    // -1 when there is nowhere to go (no eligible device, or the end was reached
    // with wrap disabled).
    int nextIndex(int currentIndex, const std::vector<bool>& eligible) const;

   private:
    CarouselSettings settings_;
    uint32_t lastInteractionMs_ = 0;
    uint32_t lastChangeMs_ = 0;
    bool everInteracted_ = false;
};

}  // namespace fp
