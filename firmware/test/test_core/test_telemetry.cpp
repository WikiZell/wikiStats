#include <unity.h>

#include <cstring>
#include <string>

#include "fp_telemetry.h"

using namespace fp;

// The document from shared/telemetry-example-linux.json, trimmed of whitespace.
static const char* kFullSample = R"JSON({
  "schema": "fleetpanel.telemetry.v1",
  "timestamp": "2026-07-28T20:45:30Z",
  "sequence": 1234,
  "device": {
    "id": "f6a3f749c2dd", "name": "WikiHermes", "hostname": "wikihermes",
    "platform": "linux", "os_name": "Debian GNU/Linux 12", "os_version": "12",
    "kernel": "6.6.31+rpt-rpi-v8", "architecture": "aarch64",
    "hardware_model": "Raspberry Pi 4 Model B Rev 1.5", "agent_version": "0.1.0"
  },
  "status": {
    "state": "online", "uptime_seconds": 348122, "boot_time": "2026-07-24T20:03:28Z",
    "process_count": 143, "logged_in_users": 1
  },
  "cpu": {
    "usage_percent": 17.4, "per_core_percent": [14.0, 22.1, 13.6, 19.8],
    "physical_cores": 4, "logical_cores": 4, "frequency_mhz": 1800.0,
    "load_1": 0.41, "load_5": 0.30, "load_15": 0.27, "temperature_c": 48.2,
    "temperatures": [{"label": "cpu", "temperature_c": 48.2, "high_c": 80.0, "critical_c": 90.0}]
  },
  "memory": {
    "total_bytes": 8589934592, "available_bytes": 5368709120, "used_bytes": 3221225472,
    "free_bytes": 4294967296, "usage_percent": 37.5, "swap_total_bytes": 1073741824,
    "swap_used_bytes": 134217728, "swap_free_bytes": 939524096, "swap_usage_percent": 12.5
  },
  "storage": {
    "total_bytes": 62538170368, "used_bytes": 27692531712, "free_bytes": 34845638656,
    "usage_percent": 44.3,
    "mounts": [{"device": "/dev/mmcblk0p2", "mountpoint": "/", "filesystem": "ext4",
                "total_bytes": 62538170368, "used_bytes": 27692531712,
                "free_bytes": 34845638656, "usage_percent": 44.3}]
  },
  "network": {
    "primary_interface": "eth0", "ip_addresses": ["192.168.1.50"],
    "rx_bytes_total": 28478212231, "tx_bytes_total": 9876241132,
    "rx_bytes_per_second": 24563.2, "tx_bytes_per_second": 4311.7
  },
  "optional": {"gpu": null, "battery": null},
  "capabilities": ["cpu","cpu_temperature","memory","swap","storage","network"]
})JSON";

static void test_parses_full_sample(void) {
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::Ok, parseTelemetry(kFullSample, strlen(kFullSample), telemetry));

    TEST_ASSERT_EQUAL_STRING("f6a3f749c2dd", telemetry.deviceId.c_str());
    TEST_ASSERT_EQUAL_STRING("WikiHermes", telemetry.name.c_str());
    TEST_ASSERT_EQUAL_STRING("linux", telemetry.platform.c_str());
    TEST_ASSERT_EQUAL_STRING("Raspberry Pi 4 Model B Rev 1.5", telemetry.hardwareModel.c_str());
    TEST_ASSERT_TRUE(telemetry.sequence.has);
    TEST_ASSERT_EQUAL_UINT32(1234, telemetry.sequence.value);

    TEST_ASSERT_TRUE(telemetry.cpuPercent.has);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 17.4f, telemetry.cpuPercent.value);
    TEST_ASSERT_EQUAL_size_t(4, telemetry.perCorePercent.size());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 22.1f, telemetry.perCorePercent[1]);
    TEST_ASSERT_TRUE(telemetry.cpuTemperature.has);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 48.2f, telemetry.cpuTemperature.value);
    TEST_ASSERT_EQUAL_size_t(1, telemetry.temperatures.size());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, telemetry.temperatures[0].critical.value);

    TEST_ASSERT_TRUE(telemetry.memTotal.has);
    TEST_ASSERT_EQUAL_UINT64(8589934592ULL, telemetry.memTotal.value);
    TEST_ASSERT_EQUAL_UINT64(62538170368ULL, telemetry.diskTotal.value);
    TEST_ASSERT_EQUAL_UINT64(28478212231ULL, telemetry.rxTotal.value);
    TEST_ASSERT_TRUE(telemetry.rxRate.has);
    TEST_ASSERT_DOUBLE_WITHIN(0.1, 24563.2, telemetry.rxRate.value);

    TEST_ASSERT_EQUAL_STRING("eth0", telemetry.primaryInterface.c_str());
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", telemetry.primaryAddress.c_str());
    TEST_ASSERT_EQUAL_size_t(1, telemetry.mounts.size());
    TEST_ASSERT_EQUAL_STRING("/", telemetry.mounts[0].mountpoint.c_str());

    TEST_ASSERT_TRUE(telemetry.hasCapability("cpu_temperature"));
    TEST_ASSERT_FALSE(telemetry.hasCapability("gpu"));
    TEST_ASSERT_FALSE(telemetry.hasGpu);
    TEST_ASSERT_FALSE(telemetry.hasBattery);
    TEST_ASSERT_EQUAL_INT64(348122, telemetry.uptimeSeconds.value);
}

static void test_unknown_fields_are_ignored(void) {
    // Forward compatibility: a v1.1 agent adds keys, firmware built today keeps working.
    const char* json = R"({"schema":"fleetpanel.telemetry.v1",
      "device":{"id":"abc123ef","name":"X","cpu_vendor":"ACME"},
      "cpu":{"usage_percent":50.0,"npu_percent":12.0},
      "brand_new_section":{"whatever":[1,2,3]},
      "capabilities":["cpu"]})";
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::Ok, parseTelemetry(json, strlen(json), telemetry));
    TEST_ASSERT_EQUAL_STRING("abc123ef", telemetry.deviceId.c_str());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, telemetry.cpuPercent.value);
}

static void test_missing_sections_leave_values_unset(void) {
    const char* json = R"({"schema":"fleetpanel.telemetry.v1","device":{"id":"abc123ef"}})";
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::Ok, parseTelemetry(json, strlen(json), telemetry));
    TEST_ASSERT_FALSE(telemetry.cpuPercent.has);
    TEST_ASSERT_FALSE(telemetry.memTotal.has);
    TEST_ASSERT_FALSE(telemetry.diskTotal.has);
    TEST_ASSERT_FALSE(telemetry.cpuTemperature.has);
    // Crucially: not zero. 0% CPU and "no reading" must not look the same.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, telemetry.cpuPercent.orDefault(0.0f));
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, telemetry.cpuPercent.orDefault(-1.0f));
}

static void test_null_is_treated_as_missing(void) {
    const char* json = R"({"schema":"fleetpanel.telemetry.v1",
      "device":{"id":"abc123ef","hardware_model":null},
      "cpu":{"usage_percent":12.0,"temperature_c":null,"temperatures":null,"frequency_mhz":null},
      "memory":{"total_bytes":null},
      "network":{"ip_addresses":null},
      "optional":{"gpu":null,"battery":null},
      "capabilities":["cpu"]})";
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::Ok, parseTelemetry(json, strlen(json), telemetry));
    TEST_ASSERT_TRUE(telemetry.cpuPercent.has);
    TEST_ASSERT_FALSE(telemetry.cpuTemperature.has);
    TEST_ASSERT_FALSE(telemetry.frequencyMhz.has);
    TEST_ASSERT_FALSE(telemetry.memTotal.has);
    TEST_ASSERT_EQUAL_size_t(0, telemetry.temperatures.size());
    TEST_ASSERT_TRUE(telemetry.hardwareModel.empty());
    TEST_ASSERT_FALSE(telemetry.hasGpu);
}

static void test_wrong_type_degrades_only_that_field(void) {
    const char* json = R"({"schema":"fleetpanel.telemetry.v1",
      "device":{"id":"abc123ef","name":42},
      "cpu":{"usage_percent":"lots","logical_cores":8},
      "capabilities":["cpu"]})";
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::Ok, parseTelemetry(json, strlen(json), telemetry));
    TEST_ASSERT_FALSE(telemetry.cpuPercent.has);
    TEST_ASSERT_TRUE(telemetry.logicalCores.has);
    TEST_ASSERT_EQUAL_INT32(8, telemetry.logicalCores.value);
    TEST_ASSERT_TRUE(telemetry.name.empty());
}

static void test_future_minor_schema_is_accepted(void) {
    const char* json = R"({"schema":"fleetpanel.telemetry.v1.4",
      "device":{"id":"abc123ef"},"cpu":{"usage_percent":9.0}})";
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::Ok, parseTelemetry(json, strlen(json), telemetry));
}

static void test_foreign_schema_is_rejected(void) {
    const char* json = R"({"schema":"someoneelse.telemetry.v1","device":{"id":"abc123ef"}})";
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::UnsupportedSchema,
                      parseTelemetry(json, strlen(json), telemetry));
}

static void test_missing_device_id_is_rejected(void) {
    const char* json = R"({"schema":"fleetpanel.telemetry.v1","device":{"name":"nameless"}})";
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::MissingDeviceId, parseTelemetry(json, strlen(json), telemetry));
}

static void test_malformed_json_is_rejected(void) {
    const char* json = R"({"schema":"fleetpanel.telemetry.v1", "device": {)";
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::InvalidJson, parseTelemetry(json, strlen(json), telemetry));
}

static void test_empty_payload_is_rejected(void) {
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::EmptyPayload, parseTelemetry(nullptr, 0, telemetry));
    TEST_ASSERT_EQUAL(ParseStatus::EmptyPayload, parseTelemetry("", 0, telemetry));
}

static void test_oversized_payload_is_refused_before_parsing(void) {
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::PayloadTooLarge,
                      parseTelemetry(kFullSample, strlen(kFullSample), telemetry, 128));
}

static void test_core_and_temperature_lists_are_bounded(void) {
    std::string json = R"({"schema":"fleetpanel.telemetry.v1","device":{"id":"abc123ef"},)";
    json += R"("cpu":{"usage_percent":5.0,"per_core_percent":[)";
    for (int i = 0; i < 128; ++i) {
        json += (i ? ",1.0" : "1.0");
    }
    json += R"(],"temperatures":[)";
    for (int i = 0; i < 40; ++i) {
        json += (i ? "," : "");
        json += R"({"label":"t","temperature_c":40.0})";
    }
    json += "]}}";

    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::Ok, parseTelemetry(json.c_str(), json.size(), telemetry, 0));
    TEST_ASSERT_EQUAL_size_t(kMaxCores, telemetry.perCorePercent.size());
    TEST_ASSERT_EQUAL_size_t(kMaxTemperatures, telemetry.temperatures.size());
}

static void test_windows_platform_parses_identically(void) {
    // The whole point of the shared protocol: a future Windows agent needs no
    // firmware change.
    const char* json = R"({"schema":"fleetpanel.telemetry.v1",
      "device":{"id":"0011aabbccdd","name":"Studio","hostname":"studio",
                "platform":"windows","os_name":"Windows 11 Pro","os_version":"10.0.26200",
                "kernel":"10.0.26200","architecture":"AMD64",
                "hardware_model":"ASUS ProArt","agent_version":"0.1.0"},
      "cpu":{"usage_percent":22.0,"temperature_c":null,"logical_cores":16},
      "memory":{"total_bytes":68719476736,"usage_percent":41.0},
      "storage":{"total_bytes":2000398934016,"usage_percent":55.0},
      "network":{"primary_interface":"Ethernet","ip_addresses":["192.168.1.77"],
                 "rx_bytes_total":10,"tx_bytes_total":5,
                 "rx_bytes_per_second":0.0,"tx_bytes_per_second":0.0},
      "capabilities":["cpu","memory","storage","network"]})";
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::Ok, parseTelemetry(json, strlen(json), telemetry));
    TEST_ASSERT_EQUAL_STRING("windows", telemetry.platform.c_str());
    TEST_ASSERT_EQUAL_STRING("Studio", telemetry.displayName().c_str());
    TEST_ASSERT_FALSE(telemetry.cpuTemperature.has);
    TEST_ASSERT_FALSE(telemetry.hasCapability("cpu_temperature"));
    TEST_ASSERT_EQUAL_INT32(16, telemetry.logicalCores.value);
}

static void test_display_name_falls_back(void) {
    const char* json = R"({"schema":"fleetpanel.telemetry.v1",
      "device":{"id":"abc123ef","hostname":"only-hostname"}})";
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::Ok, parseTelemetry(json, strlen(json), telemetry));
    TEST_ASSERT_EQUAL_STRING("only-hostname", telemetry.displayName().c_str());
}

static void test_gpu_and_battery_when_present(void) {
    const char* json = R"({"schema":"fleetpanel.telemetry.v1",
      "device":{"id":"abc123ef"},
      "optional":{"gpu":{"name":"RTX 3060","usage_percent":34.0,"temperature_c":51.0},
                  "battery":{"percent":82.5,"power_plugged":true}},
      "capabilities":["gpu","battery"]})";
    Telemetry telemetry;
    TEST_ASSERT_EQUAL(ParseStatus::Ok, parseTelemetry(json, strlen(json), telemetry));
    TEST_ASSERT_TRUE(telemetry.hasGpu);
    TEST_ASSERT_EQUAL_STRING("RTX 3060", telemetry.gpuName.c_str());
    TEST_ASSERT_TRUE(telemetry.hasBattery);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 82.5f, telemetry.batteryPercent.value);
    TEST_ASSERT_TRUE(telemetry.batteryPlugged.value);
}

void suite_telemetry(void) {
    RUN_TEST(test_parses_full_sample);
    RUN_TEST(test_unknown_fields_are_ignored);
    RUN_TEST(test_missing_sections_leave_values_unset);
    RUN_TEST(test_null_is_treated_as_missing);
    RUN_TEST(test_wrong_type_degrades_only_that_field);
    RUN_TEST(test_future_minor_schema_is_accepted);
    RUN_TEST(test_foreign_schema_is_rejected);
    RUN_TEST(test_missing_device_id_is_rejected);
    RUN_TEST(test_malformed_json_is_rejected);
    RUN_TEST(test_empty_payload_is_rejected);
    RUN_TEST(test_oversized_payload_is_refused_before_parsing);
    RUN_TEST(test_core_and_temperature_lists_are_bounded);
    RUN_TEST(test_windows_platform_parses_identically);
    RUN_TEST(test_display_name_falls_back);
    RUN_TEST(test_gpu_and_battery_when_present);
}
