// Device registry: identity, deduplication, ordering and bounded history.
//
// The same physical machine can be found three ways - mDNS, a retained MQTT `meta`
// message, and a manual entry typed by the user. All three carry the agent's stable
// `device.id`, so the registry keys on that and merges rather than accumulating
// duplicates.
//
// Merge policy, which is the part that is easy to get wrong:
//   * User-owned fields (alias, order, enabled, hidden, carousel, token) always
//     survive a rediscovery. Losing a hand-typed alias because a device rebooted
//     would be infuriating.
//   * Transport fields (base URL, path, auth mode, platform) are refreshed from
//     discovery only when the entry was itself discovered, or when the field is
//     empty. A manually entered URL is never overwritten by an mDNS record.
//   * A device found by discovery while `autoApprove` is off lands in the pending
//     list and is not displayed until the user approves it.
//
// History is a fixed-size ring of uint8 percentages per device. No allocation per
// sample, and the memory cost is known at compile time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fp_telemetry.h"

namespace fp {

constexpr size_t kHistoryPoints = 60;
constexpr uint8_t kHistoryEmpty = 0xFF;
constexpr size_t kMaxDevices = 24;

enum class DeviceSource : uint8_t {
    Manual = 0,
    Mdns,
    Mqtt,
};

enum class DeviceAuth : uint8_t {
    None = 0,
    Bearer,
    Query,
};

const char* deviceSourceName(DeviceSource source);
DeviceSource deviceSourceFromName(const std::string& name);
const char* deviceAuthName(DeviceAuth auth);
DeviceAuth deviceAuthFromName(const std::string& name);

// Persisted half of a device.
struct DeviceConfig {
    std::string id;
    std::string name;      // as reported by the agent
    std::string alias;     // set by the user; wins over `name` when non-empty
    std::string baseUrl;   // "http://192.168.1.50:8770"
    std::string path = "/api/v1/telemetry";
    std::string token;
    std::string platform = "linux";
    DeviceSource source = DeviceSource::Manual;
    DeviceAuth auth = DeviceAuth::None;
    bool enabled = true;
    bool hidden = false;
    bool carousel = true;
    uint8_t order = 0;

    const std::string& label() const { return alias.empty() ? name : alias; }
    std::string telemetryUrl() const;
};

// Fixed-capacity ring of percentage values for the sparklines.
class MetricHistory {
   public:
    void push(bool hasValue, float percent);
    void clear();
    size_t size() const { return count_; }
    // Oldest-first; returns kHistoryEmpty for gaps where the agent reported null.
    uint8_t at(size_t index) const;

   private:
    uint8_t samples_[kHistoryPoints] = {};
    size_t count_ = 0;
    size_t head_ = 0;
};

// Runtime half: the latest sample plus everything derived from it.
struct DeviceState {
    DeviceConfig config;
    Telemetry latest;
    bool everReceived = false;
    uint32_t lastContactMs = 0;
    uint32_t lastAttemptMs = 0;
    uint32_t consecutiveFailures = 0;
    std::string lastError;
    DeviceSource lastSampleSource = DeviceSource::Manual;
    // Set when an MQTT Last Will marks the agent offline. That is authoritative and
    // faster than waiting for the sample-age timer, so the UI honours it directly.
    bool announcedOffline = false;
    MetricHistory cpuHistory;
    MetricHistory ramHistory;

    uint32_t ageSeconds(uint32_t nowMs) const;
    void applySample(const Telemetry& sample, uint32_t nowMs, DeviceSource via);
    void noteFailure(const std::string& error, uint32_t nowMs);
};

class DeviceRegistry {
   public:
    // Returns true when the device was added or changed. `pending` receives entries
    // awaiting approval instead of the active list.
    bool upsertDiscovered(const DeviceConfig& incoming, bool autoApprove);
    bool upsertManual(const DeviceConfig& incoming);

    bool remove(const std::string& id);
    bool approve(const std::string& id);
    bool rejectPending(const std::string& id);
    void clearPending();

    DeviceState* find(const std::string& id);
    const DeviceState* find(const std::string& id) const;

    std::vector<DeviceState>& all() { return devices_; }
    const std::vector<DeviceState>& all() const { return devices_; }
    const std::vector<DeviceConfig>& pending() const { return pending_; }

    // Indices into `all()` for devices that should appear as dashboard pages,
    // ordered by `order` then label. Hidden and disabled devices are excluded.
    std::vector<size_t> visibleOrder() const;

    // Renumbers `order` to 0..n-1 following the given id sequence. Ids that are not
    // present are ignored; devices not listed keep their relative position at the end.
    void reorder(const std::vector<std::string>& idsInOrder);

    bool setAlias(const std::string& id, const std::string& alias);
    bool setEnabled(const std::string& id, bool enabled);
    bool setHidden(const std::string& id, bool hidden);

    size_t size() const { return devices_.size(); }
    bool full() const { return devices_.size() >= kMaxDevices; }

    void loadConfigs(const std::vector<DeviceConfig>& configs);
    std::vector<DeviceConfig> exportConfigs() const;

   private:
    bool mergeInto(DeviceState& existing, const DeviceConfig& incoming, bool fromDiscovery);
    std::vector<DeviceState> devices_;
    std::vector<DeviceConfig> pending_;
};

}  // namespace fp
