#include <unity.h>

#include "fp_units.h"

using namespace fp;

static void assertText(const char* expected, const std::string& actual) {
    TEST_ASSERT_EQUAL_STRING(expected, actual.c_str());
}

static void test_bytes_binary(void) {
    assertText("0 B", formatBytes(0));
    assertText("512 B", formatBytes(512));
    assertText("1.0 KiB", formatBytes(1024));
    assertText("1.5 KiB", formatBytes(1536));
    assertText("3.0 GiB", formatBytes(3221225472.0));
    assertText("8.0 GiB", formatBytes(8589934592.0));
    assertText("4.0 TiB", formatBytes(4.0 * 1024 * 1024 * 1024 * 1024));
}

static void test_bytes_switch_to_no_decimal_above_100(void) {
    // Keeps the numeric column the same width whether a disk is 9.8 GiB or 512 GiB.
    assertText("100 GiB", formatBytes(100.0 * 1024 * 1024 * 1024));
    assertText("512 GiB", formatBytes(512.0 * 1024 * 1024 * 1024));
}

static void test_bytes_decimal_units(void) {
    assertText("1.0 kB", formatBytes(1000, /*binary=*/false));
    assertText("1.0 MB", formatBytes(1000000, /*binary=*/false));
}

static void test_bytes_invalid(void) {
    assertText("--", formatBytes(-1));
}

static void test_rates(void) {
    assertText("0 B/s", formatRate(0));
    assertText("24.0 KiB/s", formatRate(24563.2));
    assertText("1.0 MiB/s", formatRate(1048576.0));
    assertText("2.5 GiB/s", formatRate(2.5 * 1024 * 1024 * 1024));
}

static void test_uptime(void) {
    assertText("12s", formatUptime(12));
    assertText("1m 35s", formatUptime(95));
    assertText("2h 2m", formatUptime(7325));
    assertText("4d 0h", formatUptime(348122));
    assertText("--", formatUptime(-1));
}

static void test_age(void) {
    assertText("now", formatAge(0));
    assertText("now", formatAge(1));
    assertText("12s", formatAge(12));
    assertText("4m", formatAge(250));
    assertText("2h", formatAge(7500));
    assertText("3d", formatAge(3 * 86400 + 60));
}

static void test_temperature_and_percent(void) {
    assertText("48.2 C", formatTemperature(48.2));
    assertText("17%", formatPercentWhole(17.44));
    assertText("18%", formatPercentWhole(17.6));
    assertText("17.4%", formatPercent(17.44));
}

static void test_used_of_total(void) {
    assertText("3.0 GiB / 8.0 GiB", formatUsedOfTotal(3221225472.0, 8589934592.0));
    assertText("--", formatUsedOfTotal(1.0, 0.0));
}

static void test_frequency(void) {
    assertText("1.80 GHz", formatFrequency(1800.0));
    assertText("900 MHz", formatFrequency(900.0));
    assertText("--", formatFrequency(0.0));
}

void suite_units(void) {
    RUN_TEST(test_bytes_binary);
    RUN_TEST(test_bytes_switch_to_no_decimal_above_100);
    RUN_TEST(test_bytes_decimal_units);
    RUN_TEST(test_bytes_invalid);
    RUN_TEST(test_rates);
    RUN_TEST(test_uptime);
    RUN_TEST(test_age);
    RUN_TEST(test_temperature_and_percent);
    RUN_TEST(test_used_of_total);
    RUN_TEST(test_frequency);
}
