#include "fp_units.h"

#include <cmath>
#include <cstdio>

namespace fp {

const char* const kNoValue = "--";

namespace {

constexpr const char* kBinarySuffix[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
constexpr const char* kDecimalSuffix[] = {"B", "kB", "MB", "GB", "TB", "PB"};
constexpr int kSuffixCount = 6;

std::string scaled(double value, double base, const char* const* suffixes, const char* unitTail) {
    if (!std::isfinite(value) || value < 0) {
        return kNoValue;
    }
    int index = 0;
    while (value >= base && index < kSuffixCount - 1) {
        value /= base;
        ++index;
    }
    char buffer[40];
    // One decimal below 100, none above: "9.8 GiB" and "512 GiB" occupy the same
    // width, which keeps the numeric column aligned.
    if (index == 0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f %s%s", value, suffixes[index], unitTail);
    } else if (value < 100.0) {
        std::snprintf(buffer, sizeof(buffer), "%.1f %s%s", value, suffixes[index], unitTail);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.0f %s%s", value, suffixes[index], unitTail);
    }
    return std::string(buffer);
}

}  // namespace

std::string formatBytes(double bytes, bool binary) {
    return scaled(bytes, binary ? 1024.0 : 1000.0,
                  binary ? kBinarySuffix : kDecimalSuffix, "");
}

std::string formatRate(double bytesPerSecond, bool binary) {
    return scaled(bytesPerSecond, binary ? 1024.0 : 1000.0,
                  binary ? kBinarySuffix : kDecimalSuffix, "/s");
}

std::string formatUptime(int64_t seconds) {
    if (seconds < 0) {
        return kNoValue;
    }
    const int64_t days = seconds / 86400;
    const int64_t hours = (seconds % 86400) / 3600;
    const int64_t minutes = (seconds % 3600) / 60;
    const int64_t secs = seconds % 60;

    char buffer[32];
    // Two units of precision is the sweet spot: "4d 6h" tells you what you need,
    // "4d 6h 13m 22s" does not fit next to a label.
    if (days > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lldd %lldh", (long long)days, (long long)hours);
    } else if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lldh %lldm", (long long)hours, (long long)minutes);
    } else if (minutes > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lldm %llds", (long long)minutes, (long long)secs);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llds", (long long)secs);
    }
    return std::string(buffer);
}

std::string formatAge(int64_t seconds) {
    if (seconds < 0) {
        return kNoValue;
    }
    if (seconds < 2) {
        return "now";
    }
    char buffer[24];
    if (seconds < 60) {
        std::snprintf(buffer, sizeof(buffer), "%llds", (long long)seconds);
    } else if (seconds < 3600) {
        std::snprintf(buffer, sizeof(buffer), "%lldm", (long long)(seconds / 60));
    } else if (seconds < 86400) {
        std::snprintf(buffer, sizeof(buffer), "%lldh", (long long)(seconds / 3600));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lldd", (long long)(seconds / 86400));
    }
    return std::string(buffer);
}

std::string formatTemperature(double celsius) {
    if (!std::isfinite(celsius)) {
        return kNoValue;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%.1f C", celsius);
    return std::string(buffer);
}

std::string formatPercentWhole(double percent) {
    if (!std::isfinite(percent)) {
        return kNoValue;
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%.0f%%", percent);
    return std::string(buffer);
}

std::string formatPercent(double percent) {
    if (!std::isfinite(percent)) {
        return kNoValue;
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%.1f%%", percent);
    return std::string(buffer);
}

std::string formatUsedOfTotal(double used, double total, bool binary) {
    if (!std::isfinite(used) || !std::isfinite(total) || total <= 0) {
        return kNoValue;
    }
    std::string result = formatBytes(used, binary);
    result += " / ";
    result += formatBytes(total, binary);
    return result;
}

std::string formatFrequency(double megahertz) {
    if (!std::isfinite(megahertz) || megahertz <= 0) {
        return kNoValue;
    }
    char buffer[24];
    if (megahertz >= 1000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.2f GHz", megahertz / 1000.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.0f MHz", megahertz);
    }
    return std::string(buffer);
}

}  // namespace fp
