#include "fp_devices.h"

#include <algorithm>
#include <cmath>

namespace fp {

// ------------------------------------------------------------------- naming

const char* deviceSourceName(DeviceSource source) {
    switch (source) {
        case DeviceSource::Mdns:
            return "mdns";
        case DeviceSource::Mqtt:
            return "mqtt";
        case DeviceSource::Manual:
        default:
            return "manual";
    }
}

DeviceSource deviceSourceFromName(const std::string& name) {
    if (name == "mdns") {
        return DeviceSource::Mdns;
    }
    if (name == "mqtt") {
        return DeviceSource::Mqtt;
    }
    return DeviceSource::Manual;
}

const char* deviceAuthName(DeviceAuth auth) {
    switch (auth) {
        case DeviceAuth::Bearer:
            return "bearer";
        case DeviceAuth::Query:
            return "query";
        case DeviceAuth::None:
        default:
            return "none";
    }
}

DeviceAuth deviceAuthFromName(const std::string& name) {
    if (name == "bearer" || name == "token") {
        return DeviceAuth::Bearer;
    }
    if (name == "query") {
        return DeviceAuth::Query;
    }
    return DeviceAuth::None;
}

std::string DeviceConfig::telemetryUrl() const {
    std::string url = baseUrl;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    std::string suffix = path.empty() ? std::string("/api/v1/telemetry") : path;
    if (suffix.front() != '/') {
        url.push_back('/');
    }
    url.append(suffix);
    if (auth == DeviceAuth::Query && !token.empty()) {
        url.append(url.find('?') == std::string::npos ? "?token=" : "&token=");
        url.append(token);
    }
    return url;
}

// ------------------------------------------------------------------ history

void MetricHistory::push(bool hasValue, float percent) {
    uint8_t encoded = kHistoryEmpty;
    if (hasValue && std::isfinite(percent)) {
        const float clamped = std::max(0.0f, std::min(100.0f, percent));
        encoded = static_cast<uint8_t>(clamped + 0.5f);
    }
    samples_[head_] = encoded;
    head_ = (head_ + 1) % kHistoryPoints;
    if (count_ < kHistoryPoints) {
        ++count_;
    }
}

void MetricHistory::clear() {
    count_ = 0;
    head_ = 0;
}

uint8_t MetricHistory::at(size_t index) const {
    if (index >= count_) {
        return kHistoryEmpty;
    }
    // Oldest-first: when the ring is full the oldest entry sits at head_.
    const size_t start = (count_ == kHistoryPoints) ? head_ : 0;
    return samples_[(start + index) % kHistoryPoints];
}

// ------------------------------------------------------------- device state

uint32_t DeviceState::ageSeconds(uint32_t nowMs) const {
    if (!everReceived) {
        return 0;
    }
    return (nowMs - lastContactMs) / 1000u;
}

void DeviceState::applySample(const Telemetry& sample, uint32_t nowMs, DeviceSource via) {
    latest = sample;
    everReceived = true;
    lastContactMs = nowMs;
    lastAttemptMs = nowMs;
    consecutiveFailures = 0;
    lastError.clear();
    lastSampleSource = via;
    // A fresh sample proves the agent is back regardless of what the retained Last
    // Will still says on the broker.
    announcedOffline = false;
    cpuHistory.push(sample.cpuPercent.has, sample.cpuPercent.value);
    ramHistory.push(sample.memPercent.has, sample.memPercent.value);
    // Adopt the agent's own name when the user has not overridden it, so a renamed
    // host shows up without touching the panel.
    if (config.alias.empty() && !sample.displayName().empty()) {
        config.name = sample.displayName();
    }
    if (!sample.platform.empty()) {
        config.platform = sample.platform;
    }
}

void DeviceState::noteFailure(const std::string& error, uint32_t nowMs) {
    lastAttemptMs = nowMs;
    if (consecutiveFailures < 1000000u) {
        ++consecutiveFailures;
    }
    lastError = error;
    // The last known values stay on screen; only the age changes, and the
    // stale/offline badge is derived from that.
}

// ---------------------------------------------------------------- registry

DeviceState* DeviceRegistry::find(const std::string& id) {
    for (DeviceState& device : devices_) {
        if (device.config.id == id) {
            return &device;
        }
    }
    return nullptr;
}

const DeviceState* DeviceRegistry::find(const std::string& id) const {
    for (const DeviceState& device : devices_) {
        if (device.config.id == id) {
            return &device;
        }
    }
    return nullptr;
}

bool DeviceRegistry::mergeInto(DeviceState& existing, const DeviceConfig& incoming,
                               bool fromDiscovery) {
    bool changed = false;
    DeviceConfig& current = existing.config;

    const bool mayReplaceTransport =
        !fromDiscovery || current.source != DeviceSource::Manual || current.baseUrl.empty();

    if (!incoming.name.empty() && incoming.name != current.name) {
        current.name = incoming.name;
        changed = true;
    }
    if (mayReplaceTransport && !incoming.baseUrl.empty() && incoming.baseUrl != current.baseUrl) {
        current.baseUrl = incoming.baseUrl;
        changed = true;
    }
    if (mayReplaceTransport && !incoming.path.empty() && incoming.path != current.path) {
        current.path = incoming.path;
        changed = true;
    }
    if (mayReplaceTransport && incoming.auth != current.auth) {
        current.auth = incoming.auth;
        changed = true;
    }
    if (!incoming.platform.empty() && incoming.platform != current.platform) {
        current.platform = incoming.platform;
        changed = true;
    }
    // A token is only ever set explicitly by the user, never cleared by discovery.
    if (!incoming.token.empty() && incoming.token != current.token) {
        current.token = incoming.token;
        changed = true;
    }
    if (!fromDiscovery) {
        // A manual edit is authoritative for the user-owned fields too.
        if (incoming.alias != current.alias) {
            current.alias = incoming.alias;
            changed = true;
        }
        if (incoming.enabled != current.enabled) {
            current.enabled = incoming.enabled;
            changed = true;
        }
        if (incoming.hidden != current.hidden) {
            current.hidden = incoming.hidden;
            changed = true;
        }
        if (incoming.carousel != current.carousel) {
            current.carousel = incoming.carousel;
            changed = true;
        }
        if (incoming.source == DeviceSource::Manual) {
            current.source = DeviceSource::Manual;
        }
    }
    return changed;
}

bool DeviceRegistry::upsertDiscovered(const DeviceConfig& incoming, bool autoApprove) {
    if (incoming.id.empty()) {
        return false;
    }
    if (DeviceState* existing = find(incoming.id)) {
        return mergeInto(*existing, incoming, /*fromDiscovery=*/true);
    }

    if (!autoApprove) {
        for (DeviceConfig& candidate : pending_) {
            if (candidate.id == incoming.id) {
                // Refresh the pending entry so an approval later uses the current URL.
                const bool changed = candidate.baseUrl != incoming.baseUrl ||
                                     candidate.name != incoming.name;
                candidate = incoming;
                return changed;
            }
        }
        if (pending_.size() >= kMaxDevices) {
            return false;
        }
        pending_.push_back(incoming);
        return true;
    }

    if (full()) {
        return false;
    }
    DeviceState state;
    state.config = incoming;
    state.config.order = static_cast<uint8_t>(devices_.size());
    devices_.push_back(std::move(state));
    return true;
}

bool DeviceRegistry::upsertManual(const DeviceConfig& incoming) {
    if (incoming.id.empty()) {
        return false;
    }
    if (DeviceState* existing = find(incoming.id)) {
        return mergeInto(*existing, incoming, /*fromDiscovery=*/false);
    }
    if (full()) {
        return false;
    }
    DeviceState state;
    state.config = incoming;
    state.config.source = DeviceSource::Manual;
    state.config.order = static_cast<uint8_t>(devices_.size());
    devices_.push_back(std::move(state));
    // A device the user added by hand supersedes any pending discovery of it.
    rejectPending(incoming.id);
    return true;
}

bool DeviceRegistry::remove(const std::string& id) {
    const size_t before = devices_.size();
    devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                                  [&id](const DeviceState& d) { return d.config.id == id; }),
                   devices_.end());
    return devices_.size() != before;
}

bool DeviceRegistry::approve(const std::string& id) {
    for (size_t i = 0; i < pending_.size(); ++i) {
        if (pending_[i].id != id) {
            continue;
        }
        if (full()) {
            return false;
        }
        DeviceState state;
        state.config = pending_[i];
        state.config.order = static_cast<uint8_t>(devices_.size());
        devices_.push_back(std::move(state));
        pending_.erase(pending_.begin() + static_cast<long>(i));
        return true;
    }
    return false;
}

bool DeviceRegistry::rejectPending(const std::string& id) {
    const size_t before = pending_.size();
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&id](const DeviceConfig& d) { return d.id == id; }),
                   pending_.end());
    return pending_.size() != before;
}

void DeviceRegistry::clearPending() { pending_.clear(); }

std::vector<size_t> DeviceRegistry::visibleOrder() const {
    std::vector<size_t> indices;
    indices.reserve(devices_.size());
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].config.enabled && !devices_[i].config.hidden) {
            indices.push_back(i);
        }
    }
    std::stable_sort(indices.begin(), indices.end(), [this](size_t a, size_t b) {
        const DeviceConfig& lhs = devices_[a].config;
        const DeviceConfig& rhs = devices_[b].config;
        if (lhs.order != rhs.order) {
            return lhs.order < rhs.order;
        }
        return lhs.label() < rhs.label();
    });
    return indices;
}

void DeviceRegistry::reorder(const std::vector<std::string>& idsInOrder) {
    uint8_t next = 0;
    for (const std::string& id : idsInOrder) {
        if (DeviceState* device = find(id)) {
            device->config.order = next++;
        }
    }
    // Anything the caller did not mention keeps a stable position after the listed
    // devices rather than jumping to the front.
    for (DeviceState& device : devices_) {
        const bool listed = std::find(idsInOrder.begin(), idsInOrder.end(), device.config.id) !=
                            idsInOrder.end();
        if (!listed) {
            device.config.order = next < 255 ? next++ : 255;
        }
    }
}

bool DeviceRegistry::setAlias(const std::string& id, const std::string& alias) {
    if (DeviceState* device = find(id)) {
        device->config.alias = alias;
        return true;
    }
    return false;
}

bool DeviceRegistry::setEnabled(const std::string& id, bool enabled) {
    if (DeviceState* device = find(id)) {
        device->config.enabled = enabled;
        return true;
    }
    return false;
}

bool DeviceRegistry::setHidden(const std::string& id, bool hidden) {
    if (DeviceState* device = find(id)) {
        device->config.hidden = hidden;
        return true;
    }
    return false;
}

void DeviceRegistry::loadConfigs(const std::vector<DeviceConfig>& configs) {
    devices_.clear();
    devices_.reserve(configs.size());
    for (const DeviceConfig& config : configs) {
        if (config.id.empty() || full()) {
            continue;
        }
        DeviceState state;
        state.config = config;
        devices_.push_back(std::move(state));
    }
}

std::vector<DeviceConfig> DeviceRegistry::exportConfigs() const {
    std::vector<DeviceConfig> out;
    out.reserve(devices_.size());
    for (const DeviceState& device : devices_) {
        out.push_back(device.config);
    }
    return out;
}

}  // namespace fp
