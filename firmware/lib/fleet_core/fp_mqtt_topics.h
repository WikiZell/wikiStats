// MQTT topic construction and parsing.
//
// The exact mirror of `fleetpanel_agent.mqtt.build_topics` / `parse_device_topic`.
// Both sides are unit tested against the same table in `shared/mqtt-topics.md`, so a
// change on one side that is not made on the other fails a test rather than
// producing a silently empty dashboard.
#pragma once

#include <string>

namespace fp {

struct MqttTopics {
    std::string meta;
    std::string telemetry;
    std::string availability;
};

// `baseTopic` may carry leading or trailing slashes; they are normalised away.
MqttTopics buildTopics(const std::string& baseTopic, const std::string& deviceId);

// Wildcard subscriptions the panel uses: "<base>/devices/+/<leaf>".
std::string subscriptionFor(const std::string& baseTopic, const char* leaf);

enum class TopicLeaf {
    Unknown = 0,
    Meta,
    Telemetry,
    Availability,
};

// Extracts the device ID and leaf from a received topic. Returns false for
// anything that is not a well-formed device topic under `baseTopic`.
bool parseDeviceTopic(const std::string& baseTopic, const std::string& topic,
                      std::string& deviceIdOut, TopicLeaf& leafOut);

const char* topicLeafName(TopicLeaf leaf);

std::string normaliseBaseTopic(const std::string& baseTopic);

}  // namespace fp
