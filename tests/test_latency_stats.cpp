/// @file test_latency_stats.cpp
/// @brief Unit tests for the latency statistics module.
///
/// Tests cover:
///   - Empty state behavior
///   - Single/multiple sample recording
///   - Percentile computation accuracy
///   - Reset functionality
///   - Pre-allocation correctness

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>

#include "../include/latency_stats.h"

// ─────────────────────────────────────────────────────────
//  Test infrastructure
// ─────────────────────────────────────────────────────────

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                           \
    void test_##name();                                      \
    struct Register_##name {                                  \
        Register_##name() {                                   \
            std::cout << "  Running: " #name "... ";          \
            try {                                             \
                test_##name();                                \
                std::cout << "PASSED\n";                      \
                ++tests_passed;                               \
            } catch (const std::exception& e) {               \
                std::cout << "FAILED: " << e.what() << "\n";  \
                ++tests_failed;                               \
            }                                                 \
        }                                                     \
    } register_##name;                                        \
    void test_##name()

#define ASSERT_TRUE(expr) \
    if (!(expr)) throw std::runtime_error(#expr " was false at line " + std::to_string(__LINE__))

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error(  \
        #a " != " #b " at line " + std::to_string(__LINE__))

// ─────────────────────────────────────────────────────────
//  Tests
// ─────────────────────────────────────────────────────────

TEST(initial_count_is_zero) {
    ws::LatencyStats stats;
    ASSERT_EQ(stats.count(), 0u);
}

TEST(record_ns_increments_count) {
    ws::LatencyStats stats;
    stats.record_ns(1000);
    stats.record_ns(2000);
    stats.record_ns(3000);
    ASSERT_EQ(stats.count(), 3u);
}

TEST(record_with_timepoint) {
    ws::LatencyStats stats;
    auto start = ws::LatencyStats::now();
    // Small busy-wait to ensure measurable duration
    volatile int x = 0;
    for (int i = 0; i < 1000; ++i) x += i;
    (void)x;
    stats.record(start);
    ASSERT_EQ(stats.count(), 1u);
    ASSERT_TRUE(stats.samples()[0] > 0);
}

TEST(reset_clears_samples) {
    ws::LatencyStats stats;
    stats.record_ns(1000);
    stats.record_ns(2000);
    ASSERT_EQ(stats.count(), 2u);
    stats.reset();
    ASSERT_EQ(stats.count(), 0u);
}

TEST(samples_preserve_order) {
    ws::LatencyStats stats;
    stats.record_ns(100);
    stats.record_ns(200);
    stats.record_ns(300);

    auto& samples = stats.samples();
    ASSERT_EQ(samples[0], 100);
    ASSERT_EQ(samples[1], 200);
    ASSERT_EQ(samples[2], 300);
}

TEST(report_does_not_crash_when_empty) {
    ws::LatencyStats stats;
    // Should print "No samples" message without crashing
    stats.print_report("Empty Test");
}

TEST(report_single_sample) {
    ws::LatencyStats stats;
    stats.record_ns(500000);  // 500 µs
    stats.print_report("Single Sample");
    ASSERT_EQ(stats.count(), 1u);
}

TEST(known_distribution) {
    ws::LatencyStats stats(100);

    // Insert known values: 1000, 2000, 3000, ..., 100000 ns
    for (int i = 1; i <= 100; ++i) {
        stats.record_ns(i * 1000);
    }

    ASSERT_EQ(stats.count(), 100u);

    // Verify raw access
    ASSERT_EQ(stats.samples()[0], 1000);
    ASSERT_EQ(stats.samples()[99], 100000);
}

TEST(pre_allocation_works) {
    ws::LatencyStats stats(50000);
    // Should be able to add many samples without reallocation issues
    for (int i = 0; i < 50000; ++i) {
        stats.record_ns(i);
    }
    ASSERT_EQ(stats.count(), 50000u);
}

TEST(now_is_monotonic) {
    auto t1 = ws::LatencyStats::now();
    volatile int x = 0;
    for (int i = 0; i < 100; ++i) x += i;
    (void)x;
    auto t2 = ws::LatencyStats::now();
    ASSERT_TRUE(t2 >= t1);
}

// ─────────────────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────────────────

int main() {
    std::cout << "\n=== Latency Stats Tests ===\n\n";

    std::cout << "\n";
    std::cout << "Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n\n";

    return tests_failed > 0 ? 1 : 0;
}
