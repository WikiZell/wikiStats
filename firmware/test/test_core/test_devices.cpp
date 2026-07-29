#include <unity.h>

#include "fp_devices.h"

using namespace fp;

static DeviceConfig makeConfig(const char* id, const char* name, const char* url,
                               DeviceSource source) {
    DeviceConfig config;
    config.id = id;
    config.name = name;
    config.baseUrl = url;
    config.source = source;
    return config;
}

// ------------------------------------------------------------------- dedup

static void test_same_device_found_twice_is_one_entry(void) {
    DeviceRegistry registry;
    registry.upsertDiscovered(makeConfig("f6a3f749c2dd", "WikiHermes", "http://192.168.1.50:8770",
                                         DeviceSource::Mdns),
                              /*autoApprove=*/true);
    registry.upsertDiscovered(makeConfig("f6a3f749c2dd", "WikiHermes", "http://192.168.1.50:8770",
                                         DeviceSource::Mqtt),
                              /*autoApprove=*/true);
    TEST_ASSERT_EQUAL_size_t(1, registry.size());
}

static void test_different_ids_are_different_devices(void) {
    DeviceRegistry registry;
    registry.upsertDiscovered(makeConfig("aaaa1111", "A", "http://a", DeviceSource::Mdns), true);
    registry.upsertDiscovered(makeConfig("bbbb2222", "B", "http://b", DeviceSource::Mdns), true);
    TEST_ASSERT_EQUAL_size_t(2, registry.size());
}

static void test_device_without_id_is_rejected(void) {
    DeviceRegistry registry;
    TEST_ASSERT_FALSE(registry.upsertDiscovered(makeConfig("", "X", "http://x",
                                                           DeviceSource::Mdns), true));
    TEST_ASSERT_EQUAL_size_t(0, registry.size());
}

static void test_rediscovery_keeps_user_owned_fields(void) {
    DeviceRegistry registry;
    registry.upsertManual(makeConfig("f6a3f749c2dd", "WikiHermes", "http://192.168.1.50:8770",
                                     DeviceSource::Manual));
    registry.setAlias("f6a3f749c2dd", "Kitchen Pi");
    registry.setHidden("f6a3f749c2dd", true);
    DeviceState* device = registry.find("f6a3f749c2dd");
    device->config.order = 7;
    device->config.token = "secret-token";

    registry.upsertDiscovered(makeConfig("f6a3f749c2dd", "wikihermes", "http://10.0.0.9:8770",
                                         DeviceSource::Mdns),
                              /*autoApprove=*/true);

    device = registry.find("f6a3f749c2dd");
    TEST_ASSERT_EQUAL_STRING("Kitchen Pi", device->config.alias.c_str());
    TEST_ASSERT_TRUE(device->config.hidden);
    TEST_ASSERT_EQUAL_UINT8(7, device->config.order);
    TEST_ASSERT_EQUAL_STRING("secret-token", device->config.token.c_str());
    // A hand-entered URL is never replaced by a discovery record.
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.50:8770", device->config.baseUrl.c_str());
}

static void test_rediscovery_refreshes_a_discovered_devices_address(void) {
    DeviceRegistry registry;
    registry.upsertDiscovered(makeConfig("abc12345", "Pi", "http://192.168.1.50:8770",
                                         DeviceSource::Mdns), true);
    registry.upsertDiscovered(makeConfig("abc12345", "Pi", "http://192.168.1.77:8770",
                                         DeviceSource::Mdns), true);
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.77:8770",
                             registry.find("abc12345")->config.baseUrl.c_str());
}

// --------------------------------------------------------------- approval

static void test_discovery_waits_for_approval_by_default(void) {
    DeviceRegistry registry;
    registry.upsertDiscovered(makeConfig("abc12345", "Pi", "http://p", DeviceSource::Mdns),
                              /*autoApprove=*/false);
    TEST_ASSERT_EQUAL_size_t(0, registry.size());
    TEST_ASSERT_EQUAL_size_t(1, registry.pending().size());

    TEST_ASSERT_TRUE(registry.approve("abc12345"));
    TEST_ASSERT_EQUAL_size_t(1, registry.size());
    TEST_ASSERT_EQUAL_size_t(0, registry.pending().size());
}

static void test_pending_entry_is_refreshed_not_duplicated(void) {
    DeviceRegistry registry;
    registry.upsertDiscovered(makeConfig("abc12345", "Pi", "http://old", DeviceSource::Mdns), false);
    registry.upsertDiscovered(makeConfig("abc12345", "Pi", "http://new", DeviceSource::Mdns), false);
    TEST_ASSERT_EQUAL_size_t(1, registry.pending().size());
    TEST_ASSERT_EQUAL_STRING("http://new", registry.pending()[0].baseUrl.c_str());
}

static void test_rejecting_a_pending_device(void) {
    DeviceRegistry registry;
    registry.upsertDiscovered(makeConfig("abc12345", "Pi", "http://p", DeviceSource::Mdns), false);
    TEST_ASSERT_TRUE(registry.rejectPending("abc12345"));
    TEST_ASSERT_EQUAL_size_t(0, registry.pending().size());
    TEST_ASSERT_FALSE(registry.rejectPending("abc12345"));
}

static void test_manual_add_supersedes_a_pending_discovery(void) {
    DeviceRegistry registry;
    registry.upsertDiscovered(makeConfig("abc12345", "Pi", "http://p", DeviceSource::Mdns), false);
    registry.upsertManual(makeConfig("abc12345", "Pi", "http://manual", DeviceSource::Manual));
    TEST_ASSERT_EQUAL_size_t(1, registry.size());
    TEST_ASSERT_EQUAL_size_t(0, registry.pending().size());
}

// --------------------------------------------------------------- ordering

static void test_visible_order_follows_the_order_field(void) {
    DeviceRegistry registry;
    registry.upsertManual(makeConfig("a", "Alpha", "http://a", DeviceSource::Manual));
    registry.upsertManual(makeConfig("b", "Bravo", "http://b", DeviceSource::Manual));
    registry.upsertManual(makeConfig("c", "Charlie", "http://c", DeviceSource::Manual));
    registry.find("a")->config.order = 2;
    registry.find("b")->config.order = 0;
    registry.find("c")->config.order = 1;

    const std::vector<size_t> order = registry.visibleOrder();
    TEST_ASSERT_EQUAL_size_t(3, order.size());
    TEST_ASSERT_EQUAL_STRING("b", registry.all()[order[0]].config.id.c_str());
    TEST_ASSERT_EQUAL_STRING("c", registry.all()[order[1]].config.id.c_str());
    TEST_ASSERT_EQUAL_STRING("a", registry.all()[order[2]].config.id.c_str());
}

static void test_equal_order_breaks_ties_by_label(void) {
    DeviceRegistry registry;
    registry.upsertManual(makeConfig("z", "Zulu", "http://z", DeviceSource::Manual));
    registry.upsertManual(makeConfig("a", "Alpha", "http://a", DeviceSource::Manual));
    registry.find("z")->config.order = 0;
    registry.find("a")->config.order = 0;
    const std::vector<size_t> order = registry.visibleOrder();
    TEST_ASSERT_EQUAL_STRING("Alpha", registry.all()[order[0]].config.label().c_str());
}

static void test_alias_wins_over_name_for_sorting_and_display(void) {
    DeviceRegistry registry;
    registry.upsertManual(makeConfig("a", "Zulu", "http://a", DeviceSource::Manual));
    registry.setAlias("a", "Aardvark");
    TEST_ASSERT_EQUAL_STRING("Aardvark", registry.find("a")->config.label().c_str());
}

static void test_hidden_and_disabled_devices_are_not_pages(void) {
    DeviceRegistry registry;
    registry.upsertManual(makeConfig("a", "A", "http://a", DeviceSource::Manual));
    registry.upsertManual(makeConfig("b", "B", "http://b", DeviceSource::Manual));
    registry.upsertManual(makeConfig("c", "C", "http://c", DeviceSource::Manual));
    registry.setHidden("b", true);
    registry.setEnabled("c", false);
    TEST_ASSERT_EQUAL_size_t(1, registry.visibleOrder().size());
}

static void test_reorder_renumbers_and_keeps_unlisted_devices(void) {
    DeviceRegistry registry;
    registry.upsertManual(makeConfig("a", "A", "http://a", DeviceSource::Manual));
    registry.upsertManual(makeConfig("b", "B", "http://b", DeviceSource::Manual));
    registry.upsertManual(makeConfig("c", "C", "http://c", DeviceSource::Manual));

    registry.reorder({"c", "a"});
    TEST_ASSERT_EQUAL_UINT8(0, registry.find("c")->config.order);
    TEST_ASSERT_EQUAL_UINT8(1, registry.find("a")->config.order);
    TEST_ASSERT_EQUAL_UINT8(2, registry.find("b")->config.order);
}

static void test_reorder_ignores_unknown_ids(void) {
    DeviceRegistry registry;
    registry.upsertManual(makeConfig("a", "A", "http://a", DeviceSource::Manual));
    registry.reorder({"nope", "a"});
    TEST_ASSERT_EQUAL_UINT8(0, registry.find("a")->config.order);
}

static void test_remove(void) {
    DeviceRegistry registry;
    registry.upsertManual(makeConfig("a", "A", "http://a", DeviceSource::Manual));
    TEST_ASSERT_TRUE(registry.remove("a"));
    TEST_ASSERT_FALSE(registry.remove("a"));
    TEST_ASSERT_EQUAL_size_t(0, registry.size());
}

static void test_capacity_is_bounded(void) {
    DeviceRegistry registry;
    for (size_t i = 0; i < kMaxDevices + 5; ++i) {
        DeviceConfig config;
        config.id = "id" + std::to_string(i);
        config.name = config.id;
        registry.upsertManual(config);
    }
    TEST_ASSERT_EQUAL_size_t(kMaxDevices, registry.size());
    TEST_ASSERT_TRUE(registry.full());
}

// ------------------------------------------------------------------- URLs

static void test_telemetry_url_construction(void) {
    DeviceConfig config;
    config.baseUrl = "http://192.168.1.50:8770";
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.50:8770/api/v1/telemetry",
                             config.telemetryUrl().c_str());

    config.baseUrl = "http://192.168.1.50:8770/";
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.50:8770/api/v1/telemetry",
                             config.telemetryUrl().c_str());

    config.path = "custom/path";
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.50:8770/custom/path",
                             config.telemetryUrl().c_str());
}

static void test_query_auth_appends_the_token(void) {
    DeviceConfig config;
    config.baseUrl = "http://host:8770";
    config.auth = DeviceAuth::Query;
    config.token = "abc123";
    TEST_ASSERT_EQUAL_STRING("http://host:8770/api/v1/telemetry?token=abc123",
                             config.telemetryUrl().c_str());
}

static void test_bearer_auth_leaves_the_url_clean(void) {
    DeviceConfig config;
    config.baseUrl = "http://host:8770";
    config.auth = DeviceAuth::Bearer;
    config.token = "abc123";
    TEST_ASSERT_EQUAL_STRING("http://host:8770/api/v1/telemetry", config.telemetryUrl().c_str());
}

// ----------------------------------------------------------- history ring

static void test_history_is_bounded_and_oldest_first(void) {
    MetricHistory history;
    for (int i = 0; i < 200; ++i) {
        history.push(true, static_cast<float>(i % 101));
    }
    TEST_ASSERT_EQUAL_size_t(kHistoryPoints, history.size());
    // Last value pushed was 199 % 101 = 98, and it must be the newest entry.
    TEST_ASSERT_EQUAL_UINT8(98, history.at(kHistoryPoints - 1));
    TEST_ASSERT_EQUAL_UINT8(kHistoryEmpty, history.at(kHistoryPoints));
}

static void test_history_records_gaps_for_null_readings(void) {
    MetricHistory history;
    history.push(true, 50.0f);
    history.push(false, 0.0f);
    history.push(true, 60.0f);
    TEST_ASSERT_EQUAL_UINT8(50, history.at(0));
    TEST_ASSERT_EQUAL_UINT8(kHistoryEmpty, history.at(1));
    TEST_ASSERT_EQUAL_UINT8(60, history.at(2));
}

static void test_history_clamps_out_of_range_values(void) {
    MetricHistory history;
    history.push(true, -20.0f);
    history.push(true, 300.0f);
    TEST_ASSERT_EQUAL_UINT8(0, history.at(0));
    TEST_ASSERT_EQUAL_UINT8(100, history.at(1));
}

// ------------------------------------------------------------ live state

static void test_applying_a_sample_updates_state_and_history(void) {
    DeviceState state;
    state.config.id = "abc";
    Telemetry sample;
    sample.deviceId = "abc";
    sample.name = "Pi";
    sample.platform = "linux";
    sample.cpuPercent = Opt<float>(42.0f);
    sample.memPercent = Opt<float>(61.0f);

    state.applySample(sample, 10000, DeviceSource::Mdns);
    TEST_ASSERT_TRUE(state.everReceived);
    TEST_ASSERT_EQUAL_UINT32(0, state.consecutiveFailures);
    TEST_ASSERT_EQUAL_UINT8(42, state.cpuHistory.at(0));
    TEST_ASSERT_EQUAL_UINT8(61, state.ramHistory.at(0));
    TEST_ASSERT_EQUAL_STRING("Pi", state.config.name.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, state.ageSeconds(10000));
    TEST_ASSERT_EQUAL_UINT32(20, state.ageSeconds(30000));
}

static void test_a_user_alias_survives_agent_renames(void) {
    DeviceState state;
    state.config.id = "abc";
    state.config.alias = "Kitchen Pi";
    Telemetry sample;
    sample.deviceId = "abc";
    sample.name = "renamed-host";
    state.applySample(sample, 1000, DeviceSource::Mdns);
    TEST_ASSERT_EQUAL_STRING("Kitchen Pi", state.config.label().c_str());
}

static void test_failures_keep_the_last_known_values(void) {
    DeviceState state;
    state.config.id = "abc";
    Telemetry sample;
    sample.deviceId = "abc";
    sample.cpuPercent = Opt<float>(42.0f);
    state.applySample(sample, 1000, DeviceSource::Manual);

    state.noteFailure("connection refused", 5000);
    state.noteFailure("connection refused", 9000);
    TEST_ASSERT_EQUAL_UINT32(2, state.consecutiveFailures);
    TEST_ASSERT_TRUE(state.everReceived);
    // The displayed value does not change; only its age does, which is what drives
    // the stale/offline badge.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.0f, state.latest.cpuPercent.value);
    TEST_ASSERT_EQUAL_UINT32(8, state.ageSeconds(9000));
}

// ------------------------------------------------------------ persistence

static void test_export_and_load_round_trip(void) {
    DeviceRegistry registry;
    registry.upsertManual(makeConfig("a", "A", "http://a", DeviceSource::Manual));
    registry.setAlias("a", "Alias");
    const std::vector<DeviceConfig> exported = registry.exportConfigs();

    DeviceRegistry restored;
    restored.loadConfigs(exported);
    TEST_ASSERT_EQUAL_size_t(1, restored.size());
    TEST_ASSERT_EQUAL_STRING("Alias", restored.find("a")->config.alias.c_str());
}

void suite_devices(void) {
    RUN_TEST(test_same_device_found_twice_is_one_entry);
    RUN_TEST(test_different_ids_are_different_devices);
    RUN_TEST(test_device_without_id_is_rejected);
    RUN_TEST(test_rediscovery_keeps_user_owned_fields);
    RUN_TEST(test_rediscovery_refreshes_a_discovered_devices_address);
    RUN_TEST(test_discovery_waits_for_approval_by_default);
    RUN_TEST(test_pending_entry_is_refreshed_not_duplicated);
    RUN_TEST(test_rejecting_a_pending_device);
    RUN_TEST(test_manual_add_supersedes_a_pending_discovery);
    RUN_TEST(test_visible_order_follows_the_order_field);
    RUN_TEST(test_equal_order_breaks_ties_by_label);
    RUN_TEST(test_alias_wins_over_name_for_sorting_and_display);
    RUN_TEST(test_hidden_and_disabled_devices_are_not_pages);
    RUN_TEST(test_reorder_renumbers_and_keeps_unlisted_devices);
    RUN_TEST(test_reorder_ignores_unknown_ids);
    RUN_TEST(test_remove);
    RUN_TEST(test_capacity_is_bounded);
    RUN_TEST(test_telemetry_url_construction);
    RUN_TEST(test_query_auth_appends_the_token);
    RUN_TEST(test_bearer_auth_leaves_the_url_clean);
    RUN_TEST(test_history_is_bounded_and_oldest_first);
    RUN_TEST(test_history_records_gaps_for_null_readings);
    RUN_TEST(test_history_clamps_out_of_range_values);
    RUN_TEST(test_applying_a_sample_updates_state_and_history);
    RUN_TEST(test_a_user_alias_survives_agent_renames);
    RUN_TEST(test_failures_keep_the_last_known_values);
    RUN_TEST(test_export_and_load_round_trip);
}
