// Human-readable formatting for the 320x240 panel.
//
// Everything returns a std::string built with snprintf into a small stack buffer.
// There is no operator+ on strings anywhere in the render path: repeated
// concatenation is the classic way an ESP32 UI fragments its heap over a few days
// of uptime.
#pragma once

#include <cstdint>
#include <string>

namespace fp {

// Binary units (KiB/MiB/GiB/TiB) - what every OS disk tool reports.
// 3_221_225_472 -> "3.0 GiB"
std::string formatBytes(double bytes, bool binary = true);

// 24563.2 -> "24.0 KiB/s"
std::string formatRate(double bytesPerSecond, bool binary = true);

// 348122 -> "4d 0h" ; 7325 -> "2h 2m" ; 95 -> "1m 35s" ; 12 -> "12s"
std::string formatUptime(int64_t seconds);

// Compact "time since" for the last-update indicator: "now", "12s", "4m", "2h".
std::string formatAge(int64_t seconds);

// 48.2 -> "48.2 C" (the degree sign is not in the Montserrat subset that is built in)
std::string formatTemperature(double celsius);

// 17.44 -> "17%"  (whole numbers: a 320x240 panel has no room for decimals here)
std::string formatPercentWhole(double percent);

// 17.44 -> "17.4%"
std::string formatPercent(double percent);

// "3.0 GiB / 8.0 GiB"
std::string formatUsedOfTotal(double used, double total, bool binary = true);

// Frequency: 1800.0 -> "1.80 GHz", 900.0 -> "900 MHz"
std::string formatFrequency(double megahertz);

// Placeholder for any value the agent reported as null.
extern const char* const kNoValue;

}  // namespace fp
