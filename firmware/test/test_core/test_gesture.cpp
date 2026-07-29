#include <unity.h>

#include "fp_gesture.h"

using namespace fp;

static Gesture swipe(GestureDetector& detector, int16_t fromX, int16_t toX, int16_t fromY,
                     int16_t toY, uint32_t durationMs) {
    detector.press(fromX, fromY, 1000);
    detector.move((fromX + toX) / 2, (fromY + toY) / 2, 1000 + durationMs / 2);
    detector.move(toX, toY, 1000 + durationMs);
    return detector.release(toX, toY, 1000 + durationMs);
}

static void test_clear_horizontal_swipes(void) {
    GestureDetector detector;
    TEST_ASSERT_EQUAL(Gesture::SwipeLeft, swipe(detector, 260, 60, 120, 124, 250));
    TEST_ASSERT_EQUAL(Gesture::SwipeRight, swipe(detector, 60, 260, 120, 116, 250));
}

static void test_short_horizontal_movement_is_not_a_swipe(void) {
    // 20 px of travel is a finger settling, not an intent to change device.
    GestureDetector detector;
    TEST_ASSERT_NOT_EQUAL(Gesture::SwipeLeft, swipe(detector, 160, 140, 120, 120, 200));
}

static void test_mostly_vertical_gesture_is_rejected(void) {
    // Scrolling a list must never flip the page.
    GestureDetector detector;
    TEST_ASSERT_EQUAL(Gesture::None, swipe(detector, 200, 140, 40, 200, 300));
}

static void test_diagonal_within_tolerance_still_counts(void) {
    GestureDetector detector;  // 120 px across, 60 px down -> ratio 0.5, under 0.7
    TEST_ASSERT_EQUAL(Gesture::SwipeLeft, swipe(detector, 250, 130, 100, 160, 300));
}

static void test_slow_drag_is_not_a_swipe(void) {
    GestureDetector detector;
    TEST_ASSERT_EQUAL(Gesture::None, swipe(detector, 260, 60, 120, 120, 4000));
}

static void test_tap_is_recognised(void) {
    GestureDetector detector;
    detector.press(160, 120, 5000);
    TEST_ASSERT_EQUAL(Gesture::Tap, detector.release(163, 122, 5120));
}

static void test_jitter_within_slop_is_still_a_tap(void) {
    // A resistive panel reports a few pixels of drift under changing pressure.
    GestureDetector detector;
    detector.press(160, 120, 5000);
    detector.move(166, 126, 5040);
    detector.move(158, 118, 5080);
    TEST_ASSERT_EQUAL(Gesture::Tap, detector.release(161, 121, 5150));
}

static void test_long_press_fires_once_while_held(void) {
    GestureDetector detector;
    detector.press(30, 210, 1000);
    TEST_ASSERT_FALSE(detector.pollLongPress(1200));
    TEST_ASSERT_TRUE(detector.pollLongPress(1600));
    TEST_ASSERT_FALSE(detector.pollLongPress(1700));  // only once per press
    // Releasing after a long press must not also emit a tap.
    TEST_ASSERT_EQUAL(Gesture::None, detector.release(30, 210, 1900));
}

static void test_long_press_cancelled_by_movement(void) {
    GestureDetector detector;
    detector.press(30, 210, 1000);
    detector.move(120, 210, 1100);
    TEST_ASSERT_FALSE(detector.pollLongPress(1800));
}

static void test_release_without_press_is_ignored(void) {
    GestureDetector detector;
    TEST_ASSERT_EQUAL(Gesture::None, detector.release(10, 10, 100));
}

static void test_reset_abandons_the_gesture(void) {
    GestureDetector detector;
    detector.press(260, 120, 1000);
    detector.reset();
    TEST_ASSERT_FALSE(detector.active());
    TEST_ASSERT_EQUAL(Gesture::None, detector.release(60, 120, 1200));
}

static void test_custom_minimum_distance(void) {
    GestureConfig config;
    config.minSwipeDistance = 150;
    GestureDetector detector(config);
    TEST_ASSERT_EQUAL(Gesture::None, swipe(detector, 200, 100, 120, 120, 200));
    TEST_ASSERT_EQUAL(Gesture::SwipeLeft, swipe(detector, 300, 100, 120, 120, 200));
}

static void test_gesture_names(void) {
    TEST_ASSERT_EQUAL_STRING("swipe-left", gestureName(Gesture::SwipeLeft));
    TEST_ASSERT_EQUAL_STRING("swipe-right", gestureName(Gesture::SwipeRight));
    TEST_ASSERT_EQUAL_STRING("tap", gestureName(Gesture::Tap));
    TEST_ASSERT_EQUAL_STRING("long-press", gestureName(Gesture::LongPress));
    TEST_ASSERT_EQUAL_STRING("none", gestureName(Gesture::None));
}

void suite_gesture(void) {
    RUN_TEST(test_clear_horizontal_swipes);
    RUN_TEST(test_short_horizontal_movement_is_not_a_swipe);
    RUN_TEST(test_mostly_vertical_gesture_is_rejected);
    RUN_TEST(test_diagonal_within_tolerance_still_counts);
    RUN_TEST(test_slow_drag_is_not_a_swipe);
    RUN_TEST(test_tap_is_recognised);
    RUN_TEST(test_jitter_within_slop_is_still_a_tap);
    RUN_TEST(test_long_press_fires_once_while_held);
    RUN_TEST(test_long_press_cancelled_by_movement);
    RUN_TEST(test_release_without_press_is_ignored);
    RUN_TEST(test_reset_abandons_the_gesture);
    RUN_TEST(test_custom_minimum_distance);
    RUN_TEST(test_gesture_names);
}
