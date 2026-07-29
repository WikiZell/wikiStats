#include <unity.h>

#include <cstring>
#include <string>

#include "fp_config.h"

using namespace fp;

static void test_defaults_when_nothing_is_stored(void) {
    PanelConfig config;
    const ConfigResult result = loadConfig(nullptr, 0, config);
    TEST_ASSERT_EQUAL(ConfigStatus::Defaults, result.status);
    TEST_ASSERT_TRUE(result.usable());
    TEST_ASSERT_EQUAL_UINT32(kConfigVersion, config.version);
    TEST_ASSERT_EQUAL_STRING("WikiStats", config.panel.name.c_str());
    TEST_ASSERT_EQUAL(TransportMode::Auto, config.transport.mode);
    TEST_ASSERT_FALSE(config.discovery.autoAdd);
    TEST_ASSERT_TRUE(config.carousel.enabled);
    TEST_ASSERT_EQUAL_UINT32(10, config.carousel.intervalSeconds);
}

static void test_round_trip_preserves_everything(void) {
    PanelConfig original;
    original.panel.name = "Hall Panel";
    original.panel.binaryUnits = false;
    original.wifi.hostname = "hall";
    original.wifi.addOrReplace({"HomeNet", "hunter2", 0, false});
    original.wifi.addOrReplace({"Backup", "second", 5, true});
    original.mqtt.enabled = true;
    original.mqtt.host = "broker.lan";
    original.mqtt.password = "brokerpass";
    original.transport.mode = TransportMode::Mqtt;
    original.discovery.autoAdd = true;
    original.display.brightness = 65;
    original.carousel.intervalSeconds = 25;
    original.thresholds.cpuWarn = 70.0f;
    original.web.passwordHash = "hashvalue";
    original.web.passwordSalt = "saltvalue";
    original.logging.telnetPort = 2323;

    DeviceConfig device;
    device.id = "f6a3f749c2dd";
    device.name = "WikiHermes";
    device.alias = "Kitchen";
    device.baseUrl = "http://192.168.1.50:8770";
    device.auth = DeviceAuth::Bearer;
    device.token = "devicetoken";
    device.order = 3;
    original.devices.push_back(device);
    original.sanitise();

    const std::string json = serialiseConfig(original, SecretPolicy::Include);

    PanelConfig restored;
    const ConfigResult result = loadConfig(json, restored);
    TEST_ASSERT_EQUAL(ConfigStatus::Ok, result.status);
    TEST_ASSERT_EQUAL_STRING("Hall Panel", restored.panel.name.c_str());
    TEST_ASSERT_FALSE(restored.panel.binaryUnits);
    TEST_ASSERT_EQUAL_size_t(2, restored.wifi.networks.size());
    TEST_ASSERT_EQUAL_STRING("hunter2", restored.wifi.networks[0].password.c_str());
    TEST_ASSERT_TRUE(restored.mqtt.enabled);
    TEST_ASSERT_EQUAL_STRING("brokerpass", restored.mqtt.password.c_str());
    TEST_ASSERT_EQUAL(TransportMode::Mqtt, restored.transport.mode);
    TEST_ASSERT_TRUE(restored.discovery.autoAdd);
    TEST_ASSERT_EQUAL_UINT8(65, restored.display.brightness);
    TEST_ASSERT_EQUAL_UINT32(25, restored.carousel.intervalSeconds);
    TEST_ASSERT_EQUAL_FLOAT(70.0f, restored.thresholds.cpuWarn);
    TEST_ASSERT_EQUAL_STRING("hashvalue", restored.web.passwordHash.c_str());
    TEST_ASSERT_TRUE(restored.web.passwordSet);
    TEST_ASSERT_EQUAL_UINT16(2323, restored.logging.telnetPort);
    TEST_ASSERT_EQUAL_size_t(1, restored.devices.size());
    TEST_ASSERT_EQUAL_STRING("Kitchen", restored.devices[0].alias.c_str());
    TEST_ASSERT_EQUAL_STRING("devicetoken", restored.devices[0].token.c_str());
    TEST_ASSERT_EQUAL(DeviceAuth::Bearer, restored.devices[0].auth);
    TEST_ASSERT_EQUAL_UINT8(3, restored.devices[0].order);
}

// ------------------------------------------------------------- redaction

static void test_redacted_output_contains_no_secrets(void) {
    PanelConfig config;
    config.wifi.addOrReplace({"HomeNet", "hunter2", 0, false});
    config.mqtt.password = "brokerpass";
    config.web.passwordHash = "hashvalue";
    config.web.passwordSalt = "saltvalue";
    config.ota.password = "otapass";
    DeviceConfig device;
    device.id = "abc";
    device.token = "devicetoken";
    config.devices.push_back(device);

    const std::string json = serialiseConfig(config, SecretPolicy::Redact);
    TEST_ASSERT_EQUAL(std::string::npos, json.find("hunter2"));
    TEST_ASSERT_EQUAL(std::string::npos, json.find("brokerpass"));
    TEST_ASSERT_EQUAL(std::string::npos, json.find("hashvalue"));
    TEST_ASSERT_EQUAL(std::string::npos, json.find("saltvalue"));
    TEST_ASSERT_EQUAL(std::string::npos, json.find("otapass"));
    TEST_ASSERT_EQUAL(std::string::npos, json.find("devicetoken"));
    // The SSID is not a secret and must still be visible for the UI to list it.
    TEST_ASSERT_NOT_EQUAL(std::string::npos, json.find("HomeNet"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, json.find(kRedacted));
}

static void test_unset_secret_is_reported_as_empty_not_redacted(void) {
    PanelConfig config;  // no MQTT password configured
    const std::string json = serialiseConfig(config, SecretPolicy::Redact);
    // "" tells the UI "not configured"; kRedacted would claim one exists.
    TEST_ASSERT_NOT_EQUAL(std::string::npos, json.find("\"password\":\"\""));
}

static void test_patch_with_redacted_values_keeps_the_stored_secret(void) {
    PanelConfig config;
    config.mqtt.enabled = true;
    config.mqtt.host = "broker.lan";
    config.mqtt.password = "brokerpass";
    config.wifi.addOrReplace({"HomeNet", "hunter2", 0, false});
    DeviceConfig device;
    device.id = "abc";
    device.token = "devicetoken";
    config.devices.push_back(device);

    // Exactly what a browser would PUT back after a GET.
    const std::string patch = serialiseConfig(config, SecretPolicy::Redact);
    const ConfigResult result = mergeConfigPatch(patch, config);
    TEST_ASSERT_EQUAL(ConfigStatus::Ok, result.status);
    TEST_ASSERT_EQUAL_STRING("brokerpass", config.mqtt.password.c_str());
    TEST_ASSERT_EQUAL_STRING("hunter2", config.wifi.networks[0].password.c_str());
    TEST_ASSERT_EQUAL_STRING("devicetoken", config.devices[0].token.c_str());
}

static void test_patch_can_still_set_a_new_secret(void) {
    PanelConfig config;
    config.mqtt.password = "old";
    const ConfigResult result =
        mergeConfigPatch(R"({"mqtt":{"password":"new-secret"}})", config);
    TEST_ASSERT_EQUAL(ConfigStatus::Ok, result.status);
    TEST_ASSERT_EQUAL_STRING("new-secret", config.mqtt.password.c_str());
}

static void test_patch_only_touches_supplied_keys(void) {
    PanelConfig config;
    config.panel.name = "Original";
    config.display.brightness = 90;
    mergeConfigPatch(R"({"display":{"brightness":40}})", config);
    TEST_ASSERT_EQUAL_STRING("Original", config.panel.name.c_str());
    TEST_ASSERT_EQUAL_UINT8(40, config.display.brightness);
}

// ------------------------------------------------------------- migration

static void test_migration_from_version_1(void) {
    // The pre-release layout: interval in milliseconds, one threshold per metric,
    // and `url` instead of `base_url` on devices.
    const char* legacy = R"({
      "schema":"fleetpanel.config.v1",
      "version":1,
      "panel":{"name":"Old Panel"},
      "carousel":{"enabled":true,"interval_ms":15000},
      "thresholds":{"cpu":70.0},
      "devices":[{"id":"abc12345","name":"Pi","url":"http://192.168.1.50:8770"}]
    })";

    PanelConfig config;
    const ConfigResult result = loadConfig(legacy, strlen(legacy), config);
    TEST_ASSERT_EQUAL(ConfigStatus::Migrated, result.status);
    TEST_ASSERT_EQUAL_UINT32(1, result.fromVersion);
    TEST_ASSERT_EQUAL_UINT32(kConfigVersion, config.version);

    TEST_ASSERT_EQUAL_STRING("Old Panel", config.panel.name.c_str());
    TEST_ASSERT_EQUAL_UINT32(15, config.carousel.intervalSeconds);
    TEST_ASSERT_EQUAL_FLOAT(70.0f, config.thresholds.cpuWarn);
    TEST_ASSERT_EQUAL_FLOAT(85.0f, config.thresholds.cpuCritical);
    TEST_ASSERT_EQUAL_size_t(1, config.devices.size());
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.50:8770", config.devices[0].baseUrl.c_str());
}

static void test_migrated_document_saves_at_the_current_version(void) {
    const char* legacy = R"({"schema":"fleetpanel.config.v1","version":1,
                             "carousel":{"interval_ms":9000}})";
    PanelConfig config;
    loadConfig(legacy, strlen(legacy), config);
    const std::string json = serialiseConfig(config, SecretPolicy::Include);

    PanelConfig reloaded;
    const ConfigResult result = loadConfig(json, reloaded);
    TEST_ASSERT_EQUAL(ConfigStatus::Ok, result.status);  // no second migration
    TEST_ASSERT_EQUAL_UINT32(9, reloaded.carousel.intervalSeconds);
}

static void test_config_from_newer_firmware_is_loaded_best_effort(void) {
    const char* future = R"({"schema":"fleetpanel.config.v1","version":99,
                             "panel":{"name":"From The Future"},
                             "quantum":{"entangled":true}})";
    PanelConfig config;
    const ConfigResult result = loadConfig(future, strlen(future), config);
    TEST_ASSERT_EQUAL(ConfigStatus::TooNew, result.status);
    TEST_ASSERT_TRUE(result.usable());
    // Settings this firmware understands still apply, rather than being discarded.
    TEST_ASSERT_EQUAL_STRING("From The Future", config.panel.name.c_str());
}

// ------------------------------------------------------------- robustness

static void test_malformed_json_reports_a_failure_and_yields_defaults(void) {
    PanelConfig config;
    const ConfigResult result = loadConfig(R"({"panel": {)", 11, config);
    TEST_ASSERT_EQUAL(ConfigStatus::InvalidJson, result.status);
    TEST_ASSERT_FALSE(result.usable());
    TEST_ASSERT_EQUAL_STRING("WikiStats", config.panel.name.c_str());
}

static void test_foreign_schema_is_refused(void) {
    PanelConfig config;
    const char* json = R"({"schema":"somebody.else.v9","version":1})";
    const ConfigResult result = loadConfig(json, strlen(json), config);
    TEST_ASSERT_EQUAL(ConfigStatus::WrongSchema, result.status);
    TEST_ASSERT_FALSE(result.usable());
}

static void test_unknown_keys_are_ignored(void) {
    const char* json = R"({"schema":"fleetpanel.config.v1","version":2,
                           "panel":{"name":"Panel","future_key":42},
                           "totally_new_section":{"x":1}})";
    PanelConfig config;
    const ConfigResult result = loadConfig(json, strlen(json), config);
    TEST_ASSERT_EQUAL(ConfigStatus::Ok, result.status);
    TEST_ASSERT_EQUAL_STRING("Panel", config.panel.name.c_str());
}

static void test_out_of_range_values_are_clamped_on_load(void) {
    const char* json = R"({"schema":"fleetpanel.config.v1","version":2,
      "display":{"brightness":250},
      "carousel":{"interval_s":9999},
      "transport":{"poll_interval_ms":10,"max_payload_bytes":99999999},
      "thresholds":{"cpu_warn":90,"cpu_crit":20}})";
    PanelConfig config;
    loadConfig(json, strlen(json), config);
    TEST_ASSERT_TRUE(config.display.brightness <= 100);
    TEST_ASSERT_EQUAL_UINT32(120, config.carousel.intervalSeconds);
    TEST_ASSERT_EQUAL_UINT32(1000, config.transport.pollIntervalMs);
    TEST_ASSERT_EQUAL_UINT32(65536, config.transport.maxPayloadBytes);
    TEST_ASSERT_TRUE(config.thresholds.cpuCritical >= config.thresholds.cpuWarn);
}

static void test_device_without_id_is_dropped_on_load(void) {
    const char* json = R"({"schema":"fleetpanel.config.v1","version":2,
      "devices":[{"name":"nameless","base_url":"http://x"},
                 {"id":"good1234","base_url":"http://y"}]})";
    PanelConfig config;
    loadConfig(json, strlen(json), config);
    TEST_ASSERT_EQUAL_size_t(1, config.devices.size());
    TEST_ASSERT_EQUAL_STRING("good1234", config.devices[0].id.c_str());
}

// ----------------------------------------------------------- wifi helpers

static void test_wifi_priority_order(void) {
    WifiSettings wifi;
    wifi.addOrReplace({"Third", "p", 9, false});
    wifi.addOrReplace({"First", "p", 0, false});
    wifi.addOrReplace({"Second", "p", 4, false});
    const std::vector<const WifiNetwork*> ordered = wifi.byPriority();
    TEST_ASSERT_EQUAL_STRING("First", ordered[0]->ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Second", ordered[1]->ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Third", ordered[2]->ssid.c_str());
}

static void test_wifi_add_replaces_the_same_ssid(void) {
    WifiSettings wifi;
    wifi.addOrReplace({"Net", "old", 0, false});
    wifi.addOrReplace({"Net", "new", 1, false});
    TEST_ASSERT_EQUAL_size_t(1, wifi.networks.size());
    TEST_ASSERT_EQUAL_STRING("new", wifi.networks[0].password.c_str());
}

static void test_wifi_forget_and_forget_all(void) {
    WifiSettings wifi;
    wifi.addOrReplace({"A", "p", 0, false});
    wifi.addOrReplace({"B", "p", 0, false});
    TEST_ASSERT_TRUE(wifi.forget("A"));
    TEST_ASSERT_FALSE(wifi.forget("A"));
    TEST_ASSERT_EQUAL_size_t(1, wifi.networks.size());
    wifi.forgetAll();
    TEST_ASSERT_EQUAL_size_t(0, wifi.networks.size());
}

static void test_wifi_list_is_capped(void) {
    WifiSettings wifi;
    for (size_t i = 0; i < kMaxWifiNetworks + 4; ++i) {
        wifi.addOrReplace({"net" + std::to_string(i), "p", 0, false});
    }
    TEST_ASSERT_EQUAL_size_t(kMaxWifiNetworks, wifi.networks.size());
}

static void test_empty_ssid_is_rejected(void) {
    WifiSettings wifi;
    TEST_ASSERT_FALSE(wifi.addOrReplace({"", "p", 0, false}));
    TEST_ASSERT_EQUAL_size_t(0, wifi.networks.size());
}

void suite_config(void) {
    RUN_TEST(test_defaults_when_nothing_is_stored);
    RUN_TEST(test_round_trip_preserves_everything);
    RUN_TEST(test_redacted_output_contains_no_secrets);
    RUN_TEST(test_unset_secret_is_reported_as_empty_not_redacted);
    RUN_TEST(test_patch_with_redacted_values_keeps_the_stored_secret);
    RUN_TEST(test_patch_can_still_set_a_new_secret);
    RUN_TEST(test_patch_only_touches_supplied_keys);
    RUN_TEST(test_migration_from_version_1);
    RUN_TEST(test_migrated_document_saves_at_the_current_version);
    RUN_TEST(test_config_from_newer_firmware_is_loaded_best_effort);
    RUN_TEST(test_malformed_json_reports_a_failure_and_yields_defaults);
    RUN_TEST(test_foreign_schema_is_refused);
    RUN_TEST(test_unknown_keys_are_ignored);
    RUN_TEST(test_out_of_range_values_are_clamped_on_load);
    RUN_TEST(test_device_without_id_is_dropped_on_load);
    RUN_TEST(test_wifi_priority_order);
    RUN_TEST(test_wifi_add_replaces_the_same_ssid);
    RUN_TEST(test_wifi_forget_and_forget_all);
    RUN_TEST(test_wifi_list_is_capped);
    RUN_TEST(test_empty_ssid_is_rejected);
}
