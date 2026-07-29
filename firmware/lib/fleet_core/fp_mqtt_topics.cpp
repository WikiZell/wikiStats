#include "fp_mqtt_topics.h"

namespace fp {

namespace {

constexpr const char* kDefaultBase = "fleetpanel/v1";

}  // namespace

std::string normaliseBaseTopic(const std::string& baseTopic) {
    size_t start = 0;
    size_t end = baseTopic.size();
    while (start < end && (baseTopic[start] == '/' || baseTopic[start] == ' ')) {
        ++start;
    }
    while (end > start && (baseTopic[end - 1] == '/' || baseTopic[end - 1] == ' ')) {
        --end;
    }
    std::string result = baseTopic.substr(start, end - start);
    if (result.empty()) {
        return kDefaultBase;
    }
    return result;
}

MqttTopics buildTopics(const std::string& baseTopic, const std::string& deviceId) {
    const std::string base = normaliseBaseTopic(baseTopic);
    std::string root;
    root.reserve(base.size() + deviceId.size() + 24);
    root.append(base).append("/devices/").append(deviceId);

    MqttTopics topics;
    topics.meta = root + "/meta";
    topics.telemetry = root + "/telemetry";
    topics.availability = root + "/availability";
    return topics;
}

std::string subscriptionFor(const std::string& baseTopic, const char* leaf) {
    std::string result = normaliseBaseTopic(baseTopic);
    result.append("/devices/+/");
    result.append(leaf != nullptr ? leaf : "");
    return result;
}

const char* topicLeafName(TopicLeaf leaf) {
    switch (leaf) {
        case TopicLeaf::Meta:
            return "meta";
        case TopicLeaf::Telemetry:
            return "telemetry";
        case TopicLeaf::Availability:
            return "availability";
        case TopicLeaf::Unknown:
        default:
            return "unknown";
    }
}

bool parseDeviceTopic(const std::string& baseTopic, const std::string& topic,
                      std::string& deviceIdOut, TopicLeaf& leafOut) {
    deviceIdOut.clear();
    leafOut = TopicLeaf::Unknown;

    std::string prefix = normaliseBaseTopic(baseTopic);
    prefix.append("/devices/");
    if (topic.size() <= prefix.size() || topic.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }

    const std::string remainder = topic.substr(prefix.size());
    const size_t slash = remainder.find('/');
    if (slash == std::string::npos || slash == 0) {
        return false;
    }
    const std::string deviceId = remainder.substr(0, slash);
    const std::string leaf = remainder.substr(slash + 1);
    // A nested path means this is not one of our three leaves.
    if (leaf.find('/') != std::string::npos) {
        return false;
    }

    if (leaf == "meta") {
        leafOut = TopicLeaf::Meta;
    } else if (leaf == "telemetry") {
        leafOut = TopicLeaf::Telemetry;
    } else if (leaf == "availability") {
        leafOut = TopicLeaf::Availability;
    } else {
        return false;
    }

    deviceIdOut = deviceId;
    return true;
}

}  // namespace fp
