#include "fp_thresholds.h"

#include <algorithm>
#include <cmath>

namespace fp {

namespace {

float clampf(float value, float low, float high) {
    return std::max(low, std::min(high, value));
}

uint32_t clampu(uint32_t value, uint32_t low, uint32_t high) {
    return std::max(low, std::min(high, value));
}

}  // namespace

void Thresholds::sanitise() {
    cpuWarn = clampf(cpuWarn, 1.0f, 100.0f);
    cpuCritical = clampf(cpuCritical, 1.0f, 100.0f);
    cpuTempWarn = clampf(cpuTempWarn, 20.0f, 140.0f);
    cpuTempCritical = clampf(cpuTempCritical, 20.0f, 140.0f);
    ramWarn = clampf(ramWarn, 1.0f, 100.0f);
    ramCritical = clampf(ramCritical, 1.0f, 100.0f);
    diskWarn = clampf(diskWarn, 1.0f, 100.0f);
    diskCritical = clampf(diskCritical, 1.0f, 100.0f);

    // An inverted pair would make "critical" unreachable, which is a silent failure
    // of the one job these numbers have.
    cpuCritical = std::max(cpuCritical, cpuWarn);
    cpuTempCritical = std::max(cpuTempCritical, cpuTempWarn);
    ramCritical = std::max(ramCritical, ramWarn);
    diskCritical = std::max(diskCritical, diskWarn);

    staleSeconds = clampu(staleSeconds, 1u, 3600u);
    offlineSeconds = clampu(offlineSeconds, 2u, 86400u);
    if (offlineSeconds <= staleSeconds) {
        offlineSeconds = staleSeconds + 1;
    }
}

Level levelFor(float value, float warn, float critical) {
    if (!std::isfinite(value)) {
        return Level::Unknown;
    }
    if (value >= critical) {
        return Level::Critical;
    }
    if (value >= warn) {
        return Level::Warning;
    }
    return Level::Ok;
}

Level levelForOptional(bool hasValue, float value, float warn, float critical) {
    return hasValue ? levelFor(value, warn, critical) : Level::Unknown;
}

const char* levelTag(Level level) {
    switch (level) {
        case Level::Warning:
            return "WARN";
        case Level::Critical:
            return "CRIT";
        case Level::Ok:
        case Level::Unknown:
        default:
            return "";
    }
}

const char* levelName(Level level) {
    switch (level) {
        case Level::Ok:
            return "ok";
        case Level::Warning:
            return "warning";
        case Level::Critical:
            return "critical";
        case Level::Unknown:
        default:
            return "unknown";
    }
}

Freshness freshnessFor(uint32_t ageSeconds, bool everReceived, const Thresholds& thresholds) {
    if (!everReceived) {
        return Freshness::Never;
    }
    if (ageSeconds >= thresholds.offlineSeconds) {
        return Freshness::Offline;
    }
    if (ageSeconds >= thresholds.staleSeconds) {
        return Freshness::Stale;
    }
    return Freshness::Fresh;
}

const char* freshnessName(Freshness freshness) {
    switch (freshness) {
        case Freshness::Fresh:
            return "online";
        case Freshness::Stale:
            return "stale";
        case Freshness::Offline:
            return "offline";
        case Freshness::Never:
        default:
            return "waiting";
    }
}

Level worst(Level a, Level b) {
    // Unknown is the absence of information, not a severity. It must never win over
    // a real Warning, and it must never be reported as Ok either.
    if (a == Level::Unknown) {
        return b;
    }
    if (b == Level::Unknown) {
        return a;
    }
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

}  // namespace fp
