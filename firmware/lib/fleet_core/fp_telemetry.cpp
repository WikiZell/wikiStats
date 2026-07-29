#include "fp_telemetry.h"

#include <ArduinoJson.h>

#include <cstring>

namespace fp {

namespace {

constexpr const char* kSchemaPrefix = "fleetpanel.telemetry.v1";

const std::string kEmptyString;

// ---------------------------------------------------------------- extraction

// `null`, absent and wrong-type all collapse to "no value". That is the whole
// defensive-parsing contract in three lines.
template <typename T>
void take(JsonVariantConst source, Opt<T>& out) {
    out.clear();
    if (source.isNull() || !source.template is<T>()) {
        return;
    }
    out.has = true;
    out.value = source.template as<T>();
}

// Byte counters arrive as integers, but an agent that computed one in floating
// point would otherwise lose the whole field.
void takeBytes(JsonVariantConst source, Opt<uint64_t>& out) {
    out.clear();
    if (source.isNull()) {
        return;
    }
    if (source.is<uint64_t>()) {
        out.has = true;
        out.value = source.as<uint64_t>();
        return;
    }
    if (source.is<double>()) {
        const double value = source.as<double>();
        if (value >= 0.0) {
            out.has = true;
            out.value = static_cast<uint64_t>(value);
        }
    }
}

void takeString(JsonVariantConst source, std::string& out) {
    out.clear();
    if (source.isNull() || !source.is<const char*>()) {
        return;
    }
    const char* value = source.as<const char*>();
    if (value != nullptr) {
        out.assign(value);
    }
}

// ------------------------------------------------------------------- filter

// Building this once and reusing it keeps the parse allocation proportional to the
// fields the panel actually renders, not to whatever the agent decided to send.
const JsonDocument& telemetryFilter() {
    static JsonDocument filter;
    static bool built = false;
    if (built) {
        return filter;
    }

    filter["schema"] = true;
    filter["timestamp"] = true;
    filter["sequence"] = true;
    // device and status are a handful of short scalars each; filtering inside them
    // would cost more code than it saves.
    filter["device"] = true;
    filter["status"] = true;

    JsonObject cpu = filter["cpu"].to<JsonObject>();
    cpu["usage_percent"] = true;
    cpu["physical_cores"] = true;
    cpu["logical_cores"] = true;
    cpu["frequency_mhz"] = true;
    cpu["load_1"] = true;
    cpu["load_5"] = true;
    cpu["load_15"] = true;
    cpu["temperature_c"] = true;
    cpu["per_core_percent"][0] = true;
    JsonObject temp = cpu["temperatures"][0].to<JsonObject>();
    temp["label"] = true;
    temp["temperature_c"] = true;
    temp["high_c"] = true;
    temp["critical_c"] = true;

    filter["memory"] = true;

    JsonObject storage = filter["storage"].to<JsonObject>();
    storage["total_bytes"] = true;
    storage["used_bytes"] = true;
    storage["free_bytes"] = true;
    storage["usage_percent"] = true;
    JsonObject mount = storage["mounts"][0].to<JsonObject>();
    mount["mountpoint"] = true;
    mount["filesystem"] = true;
    mount["total_bytes"] = true;
    mount["used_bytes"] = true;
    mount["free_bytes"] = true;
    mount["usage_percent"] = true;

    filter["network"] = true;

    JsonObject gpu = filter["optional"]["gpu"].to<JsonObject>();
    gpu["name"] = true;
    gpu["usage_percent"] = true;
    gpu["temperature_c"] = true;
    JsonObject battery = filter["optional"]["battery"].to<JsonObject>();
    battery["percent"] = true;
    battery["power_plugged"] = true;

    filter["capabilities"][0] = true;

    built = true;
    return filter;
}

}  // namespace

// ---------------------------------------------------------------- Telemetry

bool Telemetry::hasCapability(const char* name) const {
    if (name == nullptr) {
        return false;
    }
    for (const std::string& capability : capabilities) {
        if (capability == name) {
            return true;
        }
    }
    return false;
}

const std::string& Telemetry::displayName() const {
    if (!name.empty()) {
        return name;
    }
    if (!hostname.empty()) {
        return hostname;
    }
    return deviceId.empty() ? kEmptyString : deviceId;
}

void Telemetry::clear() {
    *this = Telemetry{};
}

// ------------------------------------------------------------------ parsing

const char* parseStatusText(ParseStatus status) {
    switch (status) {
        case ParseStatus::Ok:
            return "ok";
        case ParseStatus::EmptyPayload:
            return "empty payload";
        case ParseStatus::PayloadTooLarge:
            return "payload too large";
        case ParseStatus::InvalidJson:
            return "invalid JSON";
        case ParseStatus::UnsupportedSchema:
            return "unsupported schema";
        case ParseStatus::MissingDeviceId:
            return "missing device id";
    }
    return "unknown";
}

ParseStatus parseTelemetry(const char* json, size_t length, Telemetry& out, size_t maxBytes) {
    if (json == nullptr || length == 0) {
        return ParseStatus::EmptyPayload;
    }
    if (maxBytes != 0 && length > maxBytes) {
        // Refuse before allocating: an oversized response is the cheapest way to
        // exhaust the heap on a device with no PSRAM.
        return ParseStatus::PayloadTooLarge;
    }

    JsonDocument doc;
    const DeserializationError error =
        deserializeJson(doc, json, length, DeserializationOption::Filter(telemetryFilter()),
                        DeserializationOption::NestingLimit(12));
    if (error) {
        return ParseStatus::InvalidJson;
    }

    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull()) {
        return ParseStatus::InvalidJson;
    }

    Telemetry parsed;
    takeString(root["schema"], parsed.schema);
    // Prefix match, not equality: a future `fleetpanel.telemetry.v1.2` must still
    // render on firmware built today.
    if (parsed.schema.rfind(kSchemaPrefix, 0) != 0) {
        return ParseStatus::UnsupportedSchema;
    }

    takeString(root["timestamp"], parsed.timestamp);
    take(root["sequence"], parsed.sequence);

    JsonObjectConst device = root["device"];
    takeString(device["id"], parsed.deviceId);
    takeString(device["name"], parsed.name);
    takeString(device["hostname"], parsed.hostname);
    takeString(device["platform"], parsed.platform);
    takeString(device["os_name"], parsed.osName);
    takeString(device["os_version"], parsed.osVersion);
    takeString(device["kernel"], parsed.kernel);
    takeString(device["architecture"], parsed.architecture);
    takeString(device["hardware_model"], parsed.hardwareModel);
    takeString(device["agent_version"], parsed.agentVersion);
    if (parsed.deviceId.empty()) {
        // Without a stable ID the sample cannot be attributed to a device, and
        // merging it into the wrong one is worse than dropping it.
        return ParseStatus::MissingDeviceId;
    }

    JsonObjectConst status = root["status"];
    takeString(status["state"], parsed.state);
    take(status["uptime_seconds"], parsed.uptimeSeconds);
    takeString(status["boot_time"], parsed.bootTime);
    take(status["process_count"], parsed.processCount);
    take(status["logged_in_users"], parsed.loggedInUsers);

    JsonObjectConst cpu = root["cpu"];
    take(cpu["usage_percent"], parsed.cpuPercent);
    take(cpu["physical_cores"], parsed.physicalCores);
    take(cpu["logical_cores"], parsed.logicalCores);
    take(cpu["frequency_mhz"], parsed.frequencyMhz);
    take(cpu["load_1"], parsed.load1);
    take(cpu["load_5"], parsed.load5);
    take(cpu["load_15"], parsed.load15);
    take(cpu["temperature_c"], parsed.cpuTemperature);

    JsonArrayConst cores = cpu["per_core_percent"];
    if (!cores.isNull()) {
        parsed.perCorePercent.reserve(cores.size() < kMaxCores ? cores.size() : kMaxCores);
        for (JsonVariantConst item : cores) {
            if (parsed.perCorePercent.size() >= kMaxCores) {
                break;  // a 128-core host must not allocate 128 floats per sample
            }
            if (item.is<float>()) {
                parsed.perCorePercent.push_back(item.as<float>());
            }
        }
    }

    JsonArrayConst temps = cpu["temperatures"];
    if (!temps.isNull()) {
        for (JsonObjectConst item : temps) {
            if (parsed.temperatures.size() >= kMaxTemperatures) {
                break;
            }
            TemperatureReading reading;
            takeString(item["label"], reading.label);
            take(item["temperature_c"], reading.celsius);
            take(item["high_c"], reading.high);
            take(item["critical_c"], reading.critical);
            if (reading.celsius.has) {
                parsed.temperatures.push_back(std::move(reading));
            }
        }
    }

    JsonObjectConst memory = root["memory"];
    takeBytes(memory["total_bytes"], parsed.memTotal);
    takeBytes(memory["available_bytes"], parsed.memAvailable);
    takeBytes(memory["used_bytes"], parsed.memUsed);
    takeBytes(memory["free_bytes"], parsed.memFree);
    take(memory["usage_percent"], parsed.memPercent);
    takeBytes(memory["swap_total_bytes"], parsed.swapTotal);
    takeBytes(memory["swap_used_bytes"], parsed.swapUsed);
    take(memory["swap_usage_percent"], parsed.swapPercent);

    JsonObjectConst storage = root["storage"];
    takeBytes(storage["total_bytes"], parsed.diskTotal);
    takeBytes(storage["used_bytes"], parsed.diskUsed);
    takeBytes(storage["free_bytes"], parsed.diskFree);
    take(storage["usage_percent"], parsed.diskPercent);

    JsonArrayConst mounts = storage["mounts"];
    if (!mounts.isNull()) {
        for (JsonObjectConst item : mounts) {
            if (parsed.mounts.size() >= kMaxMounts) {
                break;
            }
            MountInfo mount;
            takeString(item["mountpoint"], mount.mountpoint);
            takeString(item["filesystem"], mount.filesystem);
            takeBytes(item["total_bytes"], mount.totalBytes);
            takeBytes(item["used_bytes"], mount.usedBytes);
            takeBytes(item["free_bytes"], mount.freeBytes);
            take(item["usage_percent"], mount.usagePercent);
            if (!mount.mountpoint.empty()) {
                parsed.mounts.push_back(std::move(mount));
            }
        }
    }

    JsonObjectConst network = root["network"];
    takeString(network["primary_interface"], parsed.primaryInterface);
    JsonArrayConst addresses = network["ip_addresses"];
    if (!addresses.isNull()) {
        for (JsonVariantConst item : addresses) {
            if (item.is<const char*>() && item.as<const char*>() != nullptr) {
                parsed.primaryAddress.assign(item.as<const char*>());
                break;  // only the first address is ever displayed
            }
        }
    }
    takeBytes(network["rx_bytes_total"], parsed.rxTotal);
    takeBytes(network["tx_bytes_total"], parsed.txTotal);
    take(network["rx_bytes_per_second"], parsed.rxRate);
    take(network["tx_bytes_per_second"], parsed.txRate);

    JsonObjectConst optional = root["optional"];
    JsonObjectConst gpu = optional["gpu"];
    if (!gpu.isNull()) {
        parsed.hasGpu = true;
        takeString(gpu["name"], parsed.gpuName);
        take(gpu["usage_percent"], parsed.gpuPercent);
        take(gpu["temperature_c"], parsed.gpuTemperature);
    }
    JsonObjectConst battery = optional["battery"];
    if (!battery.isNull()) {
        parsed.hasBattery = true;
        take(battery["percent"], parsed.batteryPercent);
        take(battery["power_plugged"], parsed.batteryPlugged);
    }

    JsonArrayConst capabilities = root["capabilities"];
    if (!capabilities.isNull()) {
        for (JsonVariantConst item : capabilities) {
            if (parsed.capabilities.size() >= 16) {
                break;
            }
            if (item.is<const char*>() && item.as<const char*>() != nullptr) {
                parsed.capabilities.emplace_back(item.as<const char*>());
            }
        }
    }

    out = std::move(parsed);
    return ParseStatus::Ok;
}

}  // namespace fp
