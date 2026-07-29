// Threshold evaluation and data freshness.
//
// Accessibility rule baked into the API: every level carries a short text tag as
// well as a colour hint. Colour alone is not a signal - the panel always draws the
// tag next to the value.
#pragma once

#include <cstdint>

namespace fp {

enum class Level : uint8_t {
    Unknown = 0,  // the agent reported null for this metric
    Ok,
    Warning,
    Critical,
};

// Freshness is decided by the panel from sample age, never from the agent's own
// `status.state`: an agent that has wedged still claims to be online.
enum class Freshness : uint8_t {
    Never = 0,  // no sample has ever arrived
    Fresh,
    Stale,
    Offline,
};

struct Thresholds {
    float cpuWarn = 80.0f;
    float cpuCritical = 95.0f;
    float cpuTempWarn = 70.0f;
    float cpuTempCritical = 85.0f;
    float ramWarn = 80.0f;
    float ramCritical = 95.0f;
    float diskWarn = 85.0f;
    float diskCritical = 95.0f;
    uint32_t staleSeconds = 15;
    uint32_t offlineSeconds = 60;

    // Clamps values into a usable range and repairs inverted pairs, so a bad edit
    // through the web UI or a restored backup cannot leave the panel unable to
    // signal a critical state.
    void sanitise();
};

Level levelFor(float value, float warn, float critical);
Level levelForOptional(bool hasValue, float value, float warn, float critical);

// "", "WARN", "CRIT" - drawn beside the number so the state does not rely on hue.
const char* levelTag(Level level);
const char* levelName(Level level);

Freshness freshnessFor(uint32_t ageSeconds, bool everReceived, const Thresholds& thresholds);
const char* freshnessName(Freshness freshness);

// The worst of several levels; Unknown never masks a real Warning or Critical.
Level worst(Level a, Level b);

}  // namespace fp
