// Defensive parser for `fleetpanel.telemetry.v1`.
//
// Rules this parser follows, because malformed input reaching a device that has no
// keyboard and no console must never turn into a boot loop:
//
//  * Unknown fields are ignored. A v1.1 agent that adds keys keeps working here.
//  * Missing fields become `Opt<T>` with `has == false`, never a default that looks
//    like real data. A CPU with no reading shows "--", not 0%.
//  * `null` is treated exactly like "missing", which is what the protocol promises.
//  * Type mismatches degrade to "missing" for that field only.
//  * Payloads above a caller-supplied ceiling are rejected before parsing.
//  * An ArduinoJson filter keeps only the fields below, so a hostile or buggy agent
//    cannot make the panel allocate an arbitrary document.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fp {

template <typename T>
struct Opt {
    bool has = false;
    T value{};

    Opt() = default;
    Opt(T v) : has(true), value(v) {}

    T orDefault(T fallback) const { return has ? value : fallback; }
    void clear() {
        has = false;
        value = T{};
    }
};

struct TemperatureReading {
    std::string label;
    Opt<float> celsius;
    Opt<float> high;
    Opt<float> critical;
};

struct MountInfo {
    std::string mountpoint;
    std::string filesystem;
    Opt<uint64_t> totalBytes;
    Opt<uint64_t> usedBytes;
    Opt<uint64_t> freeBytes;
    Opt<float> usagePercent;
};

// Caps. A panel with 12 devices cannot hold unbounded per-device data.
constexpr size_t kMaxCores = 16;
constexpr size_t kMaxTemperatures = 6;
constexpr size_t kMaxMounts = 4;

struct Telemetry {
    // ---- envelope
    std::string schema;
    std::string timestamp;
    Opt<uint32_t> sequence;

    // ---- device
    std::string deviceId;
    std::string name;
    std::string hostname;
    std::string platform;  // linux | windows | macos | freebsd | other
    std::string osName;
    std::string osVersion;
    std::string kernel;
    std::string architecture;
    std::string hardwareModel;
    std::string agentVersion;

    // ---- status
    std::string state;
    Opt<int64_t> uptimeSeconds;
    std::string bootTime;
    Opt<int32_t> processCount;
    Opt<int32_t> loggedInUsers;

    // ---- cpu
    Opt<float> cpuPercent;
    std::vector<float> perCorePercent;
    Opt<int32_t> physicalCores;
    Opt<int32_t> logicalCores;
    Opt<float> frequencyMhz;
    Opt<float> load1;
    Opt<float> load5;
    Opt<float> load15;
    Opt<float> cpuTemperature;
    std::vector<TemperatureReading> temperatures;

    // ---- memory
    Opt<uint64_t> memTotal;
    Opt<uint64_t> memAvailable;
    Opt<uint64_t> memUsed;
    Opt<uint64_t> memFree;
    Opt<float> memPercent;
    Opt<uint64_t> swapTotal;
    Opt<uint64_t> swapUsed;
    Opt<float> swapPercent;

    // ---- storage (aggregate)
    Opt<uint64_t> diskTotal;
    Opt<uint64_t> diskUsed;
    Opt<uint64_t> diskFree;
    Opt<float> diskPercent;
    std::vector<MountInfo> mounts;

    // ---- network
    std::string primaryInterface;
    std::string primaryAddress;
    Opt<uint64_t> rxTotal;
    Opt<uint64_t> txTotal;
    Opt<double> rxRate;
    Opt<double> txRate;

    // ---- optional
    bool hasGpu = false;
    std::string gpuName;
    Opt<float> gpuPercent;
    Opt<float> gpuTemperature;
    bool hasBattery = false;
    Opt<float> batteryPercent;
    Opt<bool> batteryPlugged;

    std::vector<std::string> capabilities;

    bool hasCapability(const char* name) const;
    // Alias, display name, then hostname - the first non-empty wins.
    const std::string& displayName() const;
    void clear();
};

enum class ParseStatus {
    Ok = 0,
    EmptyPayload,
    PayloadTooLarge,
    InvalidJson,
    UnsupportedSchema,
    MissingDeviceId,
};

const char* parseStatusText(ParseStatus status);

// `maxBytes` of 0 disables the size check (used by unit tests).
ParseStatus parseTelemetry(const char* json, size_t length, Telemetry& out,
                           size_t maxBytes = 16384);

inline ParseStatus parseTelemetry(const std::string& json, Telemetry& out,
                                  size_t maxBytes = 16384) {
    return parseTelemetry(json.c_str(), json.size(), out, maxBytes);
}

}  // namespace fp
