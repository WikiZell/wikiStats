#include <unity.h>

#include "fp_mqtt_topics.h"

using namespace fp;

static void test_topics_match_the_shared_contract(void) {
    const MqttTopics topics = buildTopics("fleetpanel/v1", "f6a3f749c2dd");
    TEST_ASSERT_EQUAL_STRING("fleetpanel/v1/devices/f6a3f749c2dd/meta", topics.meta.c_str());
    TEST_ASSERT_EQUAL_STRING("fleetpanel/v1/devices/f6a3f749c2dd/telemetry",
                             topics.telemetry.c_str());
    TEST_ASSERT_EQUAL_STRING("fleetpanel/v1/devices/f6a3f749c2dd/availability",
                             topics.availability.c_str());
}

static void test_base_topic_slashes_are_normalised(void) {
    const char* variants[] = {"fleetpanel/v1", "/fleetpanel/v1", "fleetpanel/v1/",
                              "/fleetpanel/v1/", " fleetpanel/v1 "};
    for (const char* base : variants) {
        TEST_ASSERT_EQUAL_STRING("fleetpanel/v1/devices/abc/meta",
                                 buildTopics(base, "abc").meta.c_str());
    }
}

static void test_empty_base_topic_falls_back_to_the_default(void) {
    TEST_ASSERT_EQUAL_STRING("fleetpanel/v1", normaliseBaseTopic("").c_str());
    TEST_ASSERT_EQUAL_STRING("fleetpanel/v1", normaliseBaseTopic("///").c_str());
}

static void test_custom_base_topic(void) {
    TEST_ASSERT_EQUAL_STRING("home/panel/devices/abc/telemetry",
                             buildTopics("home/panel", "abc").telemetry.c_str());
}

static void test_subscriptions(void) {
    TEST_ASSERT_EQUAL_STRING("fleetpanel/v1/devices/+/telemetry",
                             subscriptionFor("fleetpanel/v1", "telemetry").c_str());
    TEST_ASSERT_EQUAL_STRING("fleetpanel/v1/devices/+/meta",
                             subscriptionFor("fleetpanel/v1", "meta").c_str());
}

static void test_round_trip_parse(void) {
    std::string deviceId;
    TopicLeaf leaf = TopicLeaf::Unknown;
    const MqttTopics topics = buildTopics("fleetpanel/v1", "f6a3f749c2dd");

    TEST_ASSERT_TRUE(parseDeviceTopic("fleetpanel/v1", topics.meta, deviceId, leaf));
    TEST_ASSERT_EQUAL_STRING("f6a3f749c2dd", deviceId.c_str());
    TEST_ASSERT_EQUAL(TopicLeaf::Meta, leaf);

    TEST_ASSERT_TRUE(parseDeviceTopic("fleetpanel/v1", topics.telemetry, deviceId, leaf));
    TEST_ASSERT_EQUAL(TopicLeaf::Telemetry, leaf);

    TEST_ASSERT_TRUE(parseDeviceTopic("fleetpanel/v1", topics.availability, deviceId, leaf));
    TEST_ASSERT_EQUAL(TopicLeaf::Availability, leaf);
}

static void test_parse_rejects_foreign_and_malformed_topics(void) {
    std::string deviceId;
    TopicLeaf leaf = TopicLeaf::Unknown;
    const char* bad[] = {
        "other/v1/devices/abc/telemetry",
        "fleetpanel/v1/devices/abc",
        "fleetpanel/v1/devices//telemetry",
        "fleetpanel/v1/devices/abc/unknown",
        "fleetpanel/v1/devices/abc/telemetry/extra",
        "fleetpanel/v1/devices/",
        "",
    };
    for (const char* topic : bad) {
        TEST_ASSERT_FALSE_MESSAGE(parseDeviceTopic("fleetpanel/v1", topic, deviceId, leaf), topic);
    }
}

static void test_parse_tolerates_slashy_base_topic(void) {
    std::string deviceId;
    TopicLeaf leaf = TopicLeaf::Unknown;
    TEST_ASSERT_TRUE(parseDeviceTopic("/fleetpanel/v1/",
                                      "fleetpanel/v1/devices/abc/telemetry", deviceId, leaf));
    TEST_ASSERT_EQUAL_STRING("abc", deviceId.c_str());
}

static void test_leaf_names(void) {
    TEST_ASSERT_EQUAL_STRING("meta", topicLeafName(TopicLeaf::Meta));
    TEST_ASSERT_EQUAL_STRING("telemetry", topicLeafName(TopicLeaf::Telemetry));
    TEST_ASSERT_EQUAL_STRING("availability", topicLeafName(TopicLeaf::Availability));
    TEST_ASSERT_EQUAL_STRING("unknown", topicLeafName(TopicLeaf::Unknown));
}

void suite_topics(void) {
    RUN_TEST(test_topics_match_the_shared_contract);
    RUN_TEST(test_base_topic_slashes_are_normalised);
    RUN_TEST(test_empty_base_topic_falls_back_to_the_default);
    RUN_TEST(test_custom_base_topic);
    RUN_TEST(test_subscriptions);
    RUN_TEST(test_round_trip_parse);
    RUN_TEST(test_parse_rejects_foreign_and_malformed_topics);
    RUN_TEST(test_parse_tolerates_slashy_base_topic);
    RUN_TEST(test_leaf_names);
}
