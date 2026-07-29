#include <unity.h>

#include "fp_carousel.h"

using namespace fp;

static Carousel makeCarousel(bool enabled = true, uint32_t intervalS = 10,
                             uint32_t idleResumeS = 30, bool wrap = true) {
    CarouselSettings settings;
    settings.enabled = enabled;
    settings.intervalSeconds = intervalS;
    settings.idleResumeSeconds = idleResumeS;
    settings.wrap = wrap;
    Carousel carousel;
    carousel.setSettings(settings);
    return carousel;
}

static void test_defaults_match_the_specification(void) {
    const CarouselSettings settings;
    TEST_ASSERT_TRUE(settings.enabled);
    TEST_ASSERT_EQUAL_UINT32(10, settings.intervalSeconds);
    TEST_ASSERT_EQUAL_UINT32(30, settings.idleResumeSeconds);
    TEST_ASSERT_TRUE(settings.wrap);
    TEST_ASSERT_TRUE(settings.includeOffline);
}

static void test_interval_is_clamped_to_3_120_seconds(void) {
    CarouselSettings settings;
    settings.intervalSeconds = 1;
    settings.sanitise();
    TEST_ASSERT_EQUAL_UINT32(3, settings.intervalSeconds);
    settings.intervalSeconds = 9999;
    settings.sanitise();
    TEST_ASSERT_EQUAL_UINT32(120, settings.intervalSeconds);
}

static void test_advances_after_the_interval(void) {
    Carousel carousel = makeCarousel();
    carousel.notePageChange(0);
    TEST_ASSERT_FALSE(carousel.shouldAdvance(9000));
    TEST_ASSERT_TRUE(carousel.shouldAdvance(10000));
    TEST_ASSERT_TRUE(carousel.shouldAdvance(25000));
}

static void test_touch_pauses_immediately(void) {
    Carousel carousel = makeCarousel();
    carousel.notePageChange(0);
    carousel.noteInteraction(5000);
    TEST_ASSERT_TRUE(carousel.paused(5000));
    TEST_ASSERT_FALSE(carousel.shouldAdvance(20000));  // would have advanced otherwise
}

static void test_resumes_thirty_seconds_after_the_last_touch(void) {
    Carousel carousel = makeCarousel();
    carousel.notePageChange(0);
    carousel.noteInteraction(5000);
    TEST_ASSERT_TRUE(carousel.paused(34000));
    TEST_ASSERT_FALSE(carousel.paused(35000));
    TEST_ASSERT_TRUE(carousel.shouldAdvance(35000));
}

static void test_each_touch_restarts_the_idle_timer(void) {
    Carousel carousel = makeCarousel();
    carousel.noteInteraction(5000);
    carousel.noteInteraction(20000);
    TEST_ASSERT_TRUE(carousel.paused(40000));   // 20 s after the *second* touch
    TEST_ASSERT_FALSE(carousel.paused(50000));  // 30 s after the second touch
}

static void test_seconds_until_resume(void) {
    Carousel carousel = makeCarousel();
    carousel.noteInteraction(1000);
    TEST_ASSERT_EQUAL_UINT32(30, carousel.secondsUntilResume(1000));
    TEST_ASSERT_EQUAL_UINT32(20, carousel.secondsUntilResume(11000));
    TEST_ASSERT_EQUAL_UINT32(0, carousel.secondsUntilResume(31000));
    TEST_ASSERT_EQUAL_UINT32(0, carousel.secondsUntilResume(99000));
}

static void test_disabled_carousel_never_advances(void) {
    Carousel carousel = makeCarousel(/*enabled=*/false);
    carousel.notePageChange(0);
    TEST_ASSERT_TRUE(carousel.paused(1000000));
    TEST_ASSERT_FALSE(carousel.shouldAdvance(1000000));
}

static void test_zero_idle_delay_resumes_at_once(void) {
    Carousel carousel = makeCarousel(true, 10, /*idleResumeS=*/0);
    carousel.notePageChange(0);
    carousel.noteInteraction(5000);
    TEST_ASSERT_FALSE(carousel.paused(5000));
}

static void test_next_index_wraps(void) {
    Carousel carousel = makeCarousel(true, 10, 30, /*wrap=*/true);
    const std::vector<bool> eligible = {true, true, true};
    TEST_ASSERT_EQUAL_INT(1, carousel.nextIndex(0, eligible));
    TEST_ASSERT_EQUAL_INT(2, carousel.nextIndex(1, eligible));
    TEST_ASSERT_EQUAL_INT(0, carousel.nextIndex(2, eligible));
}

static void test_next_index_stops_at_the_end_without_wrap(void) {
    Carousel carousel = makeCarousel(true, 10, 30, /*wrap=*/false);
    const std::vector<bool> eligible = {true, true, true};
    TEST_ASSERT_EQUAL_INT(1, carousel.nextIndex(0, eligible));
    TEST_ASSERT_EQUAL_INT(-1, carousel.nextIndex(2, eligible));
}

static void test_next_index_skips_ineligible_devices(void) {
    Carousel carousel = makeCarousel();
    const std::vector<bool> eligible = {true, false, false, true};
    TEST_ASSERT_EQUAL_INT(3, carousel.nextIndex(0, eligible));
    TEST_ASSERT_EQUAL_INT(0, carousel.nextIndex(3, eligible));
}

static void test_single_eligible_device_is_not_an_advance(void) {
    // Re-rendering the same page every 10 s would restart animations for no reason.
    Carousel carousel = makeCarousel();
    const std::vector<bool> eligible = {false, true, false};
    TEST_ASSERT_EQUAL_INT(-1, carousel.nextIndex(1, eligible));
}

static void test_no_eligible_devices(void) {
    Carousel carousel = makeCarousel();
    TEST_ASSERT_EQUAL_INT(-1, carousel.nextIndex(0, {false, false}));
    TEST_ASSERT_EQUAL_INT(-1, carousel.nextIndex(0, {}));
}

static void test_out_of_range_current_index_selects_the_first_eligible(void) {
    Carousel carousel = makeCarousel();
    const std::vector<bool> eligible = {false, true, true};
    TEST_ASSERT_EQUAL_INT(1, carousel.nextIndex(-1, eligible));
    TEST_ASSERT_EQUAL_INT(1, carousel.nextIndex(99, eligible));
}

void suite_carousel(void) {
    RUN_TEST(test_defaults_match_the_specification);
    RUN_TEST(test_interval_is_clamped_to_3_120_seconds);
    RUN_TEST(test_advances_after_the_interval);
    RUN_TEST(test_touch_pauses_immediately);
    RUN_TEST(test_resumes_thirty_seconds_after_the_last_touch);
    RUN_TEST(test_each_touch_restarts_the_idle_timer);
    RUN_TEST(test_seconds_until_resume);
    RUN_TEST(test_disabled_carousel_never_advances);
    RUN_TEST(test_zero_idle_delay_resumes_at_once);
    RUN_TEST(test_next_index_wraps);
    RUN_TEST(test_next_index_stops_at_the_end_without_wrap);
    RUN_TEST(test_next_index_skips_ineligible_devices);
    RUN_TEST(test_single_eligible_device_is_not_an_advance);
    RUN_TEST(test_no_eligible_devices);
    RUN_TEST(test_out_of_range_current_index_selects_the_first_eligible);
}
