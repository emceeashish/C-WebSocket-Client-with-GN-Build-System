/// @file test_spsc_queue.cpp
/// @brief Unit tests for the lock-free SPSC ring buffer.
///
/// Tests cover:
///   - Basic push/pop operations
///   - Empty/full boundary conditions
///   - Single-threaded correctness
///   - Multi-threaded producer-consumer correctness
///   - Move semantics
///   - Capacity and size tracking

#include <atomic>
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../include/spsc_queue.h"

// ─────────────────────────────────────────────────────────
//  Test infrastructure (minimal, no external dependency)
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

#define ASSERT_FALSE(expr) \
    if (expr) throw std::runtime_error(#expr " was true at line " + std::to_string(__LINE__))

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error(  \
        #a " != " #b " at line " + std::to_string(__LINE__))

// ─────────────────────────────────────────────────────────
//  Tests
// ─────────────────────────────────────────────────────────

TEST(empty_queue_returns_nullopt) {
    ws::SPSCQueue<int, 4> q;
    auto result = q.try_pop();
    ASSERT_FALSE(result.has_value());
    ASSERT_TRUE(q.empty());
}

TEST(push_pop_single_element) {
    ws::SPSCQueue<int, 4> q;
    ASSERT_TRUE(q.try_push(42));
    auto result = q.try_pop();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result, 42);
    ASSERT_TRUE(q.empty());
}

TEST(push_pop_multiple_elements) {
    ws::SPSCQueue<int, 8> q;
    for (int i = 0; i < 7; ++i) {
        ASSERT_TRUE(q.try_push(i));
    }
    for (int i = 0; i < 7; ++i) {
        auto result = q.try_pop();
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(*result, i);
    }
    ASSERT_TRUE(q.empty());
}

TEST(queue_full_returns_false) {
    ws::SPSCQueue<int, 4> q;
    ASSERT_TRUE(q.try_push(1));
    ASSERT_TRUE(q.try_push(2));
    ASSERT_TRUE(q.try_push(3));
    // Queue of capacity 4 can hold 3 elements (one slot reserved)
    ASSERT_FALSE(q.try_push(4));
}

TEST(fifo_order_preserved) {
    ws::SPSCQueue<int, 16> q;
    for (int i = 0; i < 15; ++i) {
        q.try_push(i * 10);
    }
    for (int i = 0; i < 15; ++i) {
        auto result = q.try_pop();
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(*result, i * 10);
    }
}

TEST(wrap_around_produces_correct_order) {
    ws::SPSCQueue<int, 4> q;
    // Fill and drain to advance indices past capacity
    for (int cycle = 0; cycle < 10; ++cycle) {
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(q.try_push(cycle * 100 + i));
        }
        for (int i = 0; i < 3; ++i) {
            auto val = q.try_pop();
            ASSERT_TRUE(val.has_value());
            ASSERT_EQ(*val, cycle * 100 + i);
        }
    }
}

TEST(capacity_returns_correct_value) {
    ws::SPSCQueue<int, 64> q;
    ASSERT_EQ(q.capacity(), 64u);
}

TEST(size_approx_tracks_correctly) {
    ws::SPSCQueue<int, 8> q;
    ASSERT_EQ(q.size_approx(), 0u);
    q.try_push(1);
    ASSERT_EQ(q.size_approx(), 1u);
    q.try_push(2);
    ASSERT_EQ(q.size_approx(), 2u);
    q.try_pop();
    ASSERT_EQ(q.size_approx(), 1u);
}

TEST(move_semantics_work) {
    ws::SPSCQueue<std::string, 4> q;
    std::string msg = "hello world";
    ASSERT_TRUE(q.try_push(std::move(msg)));
    auto result = q.try_pop();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result, "hello world");
}

TEST(concurrent_producer_consumer) {
    constexpr int kNumMessages = 100000;
    ws::SPSCQueue<int, 1024> q;
    std::atomic<bool> done{false};
    std::atomic<int> sum{0};

    // Producer thread
    std::thread producer([&] {
        for (int i = 0; i < kNumMessages; ++i) {
            while (!q.try_push(i)) {
                // Spin until space available
            }
        }
        done.store(true, std::memory_order_release);
    });

    // Consumer (main thread)
    int expected = 0;
    int64_t local_sum = 0;
    while (expected < kNumMessages) {
        if (auto val = q.try_pop()) {
            ASSERT_EQ(*val, expected);  // FIFO order must be preserved
            local_sum += *val;
            ++expected;
        }
    }

    producer.join();

    // Verify all messages received in order
    int64_t expected_sum = static_cast<int64_t>(kNumMessages - 1) * kNumMessages / 2;
    ASSERT_EQ(local_sum, expected_sum);
}

// ─────────────────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────────────────

int main() {
    std::cout << "\n=== SPSC Queue Tests ===\n\n";
    // Tests auto-register and run via static initialization

    std::cout << "\n";
    std::cout << "Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n\n";

    return tests_failed > 0 ? 1 : 0;
}
