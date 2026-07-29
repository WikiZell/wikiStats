#include <unity.h>

#include <cmath>

#include "fp_thresholds.h"

using namespace fp;

static void test_default_thresholds_match_the_specification(void) {
    const Thresholds t;
    TEST_ASSERT_EQUAL_FLOAT(80.0f, t.cpuWarn);
    TEST_ASSERT_EQUAL_FLOAT(95.0f, t.cpuCritical);
    TEST_ASSERT_EQUAL_FLOAT(70.0f, t.cpuTempWarn);
    TEST_ASSERT_EQUAL_FLOAT(85.0f, t.cpuTempCritical);
    TEST_ASSERT_EQUAL_FLOAT(80.0f, t.ramWarn);
    TEST_ASSERT_EQUAL_FLOAT(95.0f, t.ramCritical);
    TEST_ASSERT_EQUAL_FLOAT(85.0f, t.diskWarn);
    TEST_ASSERT_EQUAL_FLOAT(95.0f, t.diskCritical);
    TEST_ASSERT_EQUAL_UINT32(15, t.staleSeconds);
    TEST_ASSERT_EQUAL_UINT32(60, t.offlineSeconds);
}

static void test_level_boundaries_are_inclusive(void) {
    TEST_ASSERT_EQUAL(Level::Ok, levelFor(79.9f, 80.0f, 95.0f));
    TEST_ASSERT_EQUAL(Level::Warning, levelFor(80.0f, 80.0f, 95.0f));
    TEST_ASSERT_EQUAL(Level::Warning, levelFor(94.9f, 80.0f, 95.0f));
    TEST_ASSERT_EQUAL(Level::Critical, levelFor(95.0f, 80.0f, 95.0f));
    TEST_ASSERT_EQUAL(Level::Critical, levelFor(100.0f, 80.0f, 95.0f));
}

static void test_nan_is_unknown_not_ok(void) {
    TEST_ASSERT_EQUAL(Level::Unknown, levelFor(NAN, 80.0f, 95.0f));
}

static void test_optional_without_value_is_unknown(void) {
    TEST_ASSERT_EQUAL(Level::Unknown, levelForOptional(false, 99.0f, 80.0f, 95.0f));
    TEST_ASSERT_EQUAL(Level::Critical, levelForOptional(true, 99.0f, 80.0f, 95.0f));
}

static void test_level_tags_are_not_colour_only(void) {
    TEST_ASSERT_EQUAL_STRING("", levelTag(Level::Ok));
    TEST_ASSERT_EQUAL_STRING("", levelTag(Level::Unknown));
    TEST_ASSERT_EQUAL_STRING("WARN", levelTag(Level::Warning));
    TEST_ASSERT_EQUAL_STRING("CRIT", levelTag(Level::Critical));
}

static void test_worst_ignores_unknown(void) {
    TEST_ASSERT_EQUAL(Level::Warning, worst(Level::Unknown, Level::Warning));
    TEST_ASSERT_EQUAL(Level::Warning, worst(Level::Warning, Level::Unknown));
    TEST_ASSERT_EQUAL(Level::Critical, worst(Level::Warning, Level::Critical));
    TEST_ASSERT_EQUAL(Level::Ok, worst(Level::Ok, Level::Ok));
    TEST_ASSERT_EQUAL(Level::Unknown, worst(Level::Unknown, Level::Unknown));
}

static void test_freshness_transitions(void) {
    Thresholds t;  // stale at 15 s, offline at 60 s
    TEST_ASSERT_EQUAL(Freshness::Never, freshnessFor(0, false, t));
    TEST_ASSERT_EQUAL(Freshness::Fresh, freshnessFor(0, true, t));
    TEST_ASSERT_EQUAL(Freshness::Fresh, freshnessFor(14, true, t));
    TEST_ASSERT_EQUAL(Freshness::Stale, freshnessFor(15, true, t));
    TEST_ASSERT_EQUAL(Freshness::Stale, freshnessFor(59, true, t));
    TEST_ASSERT_EQUAL(Freshness::Offline, freshnessFor(60, true, t));
    TEST_ASSERT_EQUAL(Freshness::Offline, freshnessFor(100000, true, t));
}

static void test_freshness_names(void) {
    TEST_ASSERT_EQUAL_STRING("online", freshnessName(Freshness::Fresh));
    TEST_ASSERT_EQUAL_STRING("stale", freshnessName(Freshness::Stale));
    TEST_ASSERT_EQUAL_STRING("offline", freshnessName(Freshness::Offline));
    TEST_ASSERT_EQUAL_STRING("waiting", freshnessName(Freshness::Never));
}

static void test_sanitise_repairs_inverted_pairs(void) {
    Thresholds t;
    t.cpuWarn = 90.0f;
    t.cpuCritical = 50.0f;  // would make "critical" unreachable
    t.ramWarn = 99.0f;
    t.ramCritical = 10.0f;
    t.sanitise();
    TEST_ASSERT_TRUE(t.cpuCritical >= t.cpuWarn);
    TEST_ASSERT_TRUE(t.ramCritical >= t.ramWarn);
}

static void test_sanitise_clamps_out_of_range_values(void) {
    Thresholds t;
    t.cpuWarn = -5.0f;
    t.diskCritical = 5000.0f;
    t.cpuTempWarn = 0.0f;
    t.staleSeconds = 0;
    t.offlineSeconds = 0;
    t.sanitise();
    TEST_ASSERT_TRUE(t.cpuWarn >= 1.0f);
    TEST_ASSERT_TRUE(t.diskCritical <= 100.0f);
    TEST_ASSERT_TRUE(t.cpuTempWarn >= 20.0f);
    TEST_ASSERT_TRUE(t.staleSeconds >= 1);
    TEST_ASSERT_TRUE(t.offlineSeconds > t.staleSeconds);
}

static void test_sanitise_keeps_offline_after_stale(void) {
    Thresholds t;
    t.staleSeconds = 90;
    t.offlineSeconds = 30;
    t.sanitise();
    TEST_ASSERT_TRUE(t.offlineSeconds > t.staleSeconds);
}

void suite_thresholds(void) {
    RUN_TEST(test_default_thresholds_match_the_specification);
    RUN_TEST(test_level_boundaries_are_inclusive);
    RUN_TEST(test_nan_is_unknown_not_ok);
    RUN_TEST(test_optional_without_value_is_unknown);
    RUN_TEST(test_level_tags_are_not_colour_only);
    RUN_TEST(test_worst_ignores_unknown);
    RUN_TEST(test_freshness_transitions);
    RUN_TEST(test_freshness_names);
    RUN_TEST(test_sanitise_repairs_inverted_pairs);
    RUN_TEST(test_sanitise_clamps_out_of_range_values);
    RUN_TEST(test_sanitise_keeps_offline_after_stale);
}
