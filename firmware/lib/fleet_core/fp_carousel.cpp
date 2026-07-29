#include "fp_carousel.h"

#include <algorithm>

namespace fp {

void CarouselSettings::sanitise() {
    intervalSeconds = std::max<uint32_t>(3, std::min<uint32_t>(120, intervalSeconds));
    idleResumeSeconds = std::min<uint32_t>(3600, idleResumeSeconds);
}

void Carousel::setSettings(const CarouselSettings& settings) {
    settings_ = settings;
    settings_.sanitise();
}

void Carousel::noteInteraction(uint32_t nowMs) {
    lastInteractionMs_ = nowMs;
    everInteracted_ = true;
}

void Carousel::notePageChange(uint32_t nowMs) { lastChangeMs_ = nowMs; }

bool Carousel::paused(uint32_t nowMs) const {
    if (!settings_.enabled) {
        return true;
    }
    if (!everInteracted_) {
        return false;
    }
    // Unsigned subtraction handles the 49-day millis() rollover correctly as long as
    // the interval is far smaller than the wrap period, which it always is here.
    return (nowMs - lastInteractionMs_) < (settings_.idleResumeSeconds * 1000u);
}

uint32_t Carousel::secondsUntilResume(uint32_t nowMs) const {
    if (!settings_.enabled || !everInteracted_) {
        return 0;
    }
    const uint32_t elapsed = nowMs - lastInteractionMs_;
    const uint32_t window = settings_.idleResumeSeconds * 1000u;
    if (elapsed >= window) {
        return 0;
    }
    return (window - elapsed + 999u) / 1000u;
}

bool Carousel::shouldAdvance(uint32_t nowMs) const {
    if (paused(nowMs)) {
        return false;
    }
    return (nowMs - lastChangeMs_) >= (settings_.intervalSeconds * 1000u);
}

int Carousel::nextIndex(int currentIndex, const std::vector<bool>& eligible) const {
    const int count = static_cast<int>(eligible.size());
    if (count <= 0) {
        return -1;
    }
    if (currentIndex < 0 || currentIndex >= count) {
        for (int i = 0; i < count; ++i) {
            if (eligible[static_cast<size_t>(i)]) {
                return i;
            }
        }
        return -1;
    }

    if (settings_.wrap) {
        for (int step = 1; step <= count; ++step) {
            const int candidate = (currentIndex + step) % count;
            if (eligible[static_cast<size_t>(candidate)]) {
                // Landing back on the current page is not an advance.
                return candidate == currentIndex ? -1 : candidate;
            }
        }
        return -1;
    }

    for (int candidate = currentIndex + 1; candidate < count; ++candidate) {
        if (eligible[static_cast<size_t>(candidate)]) {
            return candidate;
        }
    }
    return -1;
}

}  // namespace fp
