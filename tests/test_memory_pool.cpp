/// @file test_memory_pool.cpp
/// @brief Unit tests for the fixed-block memory pool.
///
/// Tests cover:
///   - Basic allocate/deallocate
///   - Pool exhaustion
///   - Ownership verification
///   - RAII PoolPtr correctness
///   - Full cycle (exhaust + refill)

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../include/memory_pool.h"

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

#define ASSERT_FALSE(expr) \
    if (expr) throw std::runtime_error(#expr " was true at line " + std::to_string(__LINE__))

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error(  \
        #a " != " #b " at line " + std::to_string(__LINE__))

#define ASSERT_NE(a, b) \
    if ((a) == (b)) throw std::runtime_error(  \
        #a " == " #b " at line " + std::to_string(__LINE__))

// ─────────────────────────────────────────────────────────
//  Tests
// ─────────────────────────────────────────────────────────

TEST(initial_state) {
    ws::MemoryPool<64, 16> pool;
    ASSERT_EQ(pool.free_count(), 16u);
    ASSERT_EQ(pool.capacity(), 16u);
    ASSERT_EQ(pool.block_size(), 64u);
}

TEST(allocate_returns_non_null) {
    ws::MemoryPool<64, 4> pool;
    void* ptr = pool.allocate();
    ASSERT_NE(ptr, nullptr);
    ASSERT_EQ(pool.free_count(), 3u);
    pool.deallocate(ptr);
}

TEST(deallocate_restores_count) {
    ws::MemoryPool<64, 4> pool;
    void* ptr = pool.allocate();
    ASSERT_EQ(pool.free_count(), 3u);
    pool.deallocate(ptr);
    ASSERT_EQ(pool.free_count(), 4u);
}

TEST(exhaust_pool) {
    ws::MemoryPool<64, 4> pool;
    std::vector<void*> ptrs;

    for (int i = 0; i < 4; ++i) {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    ASSERT_EQ(pool.free_count(), 0u);

    // Next allocation should return nullptr
    ASSERT_EQ(pool.allocate(), nullptr);

    // Free all
    for (auto* p : ptrs) {
        pool.deallocate(p);
    }
    ASSERT_EQ(pool.free_count(), 4u);
}

TEST(unique_pointers) {
    ws::MemoryPool<64, 8> pool;
    std::vector<void*> ptrs;

    for (int i = 0; i < 8; ++i) {
        ptrs.push_back(pool.allocate());
    }

    // All pointers should be distinct
    for (size_t i = 0; i < ptrs.size(); ++i) {
        for (size_t j = i + 1; j < ptrs.size(); ++j) {
            ASSERT_NE(ptrs[i], ptrs[j]);
        }
    }

    for (auto* p : ptrs) {
        pool.deallocate(p);
    }
}

TEST(ownership_check) {
    ws::MemoryPool<64, 4> pool;
    void* ptr = pool.allocate();
    ASSERT_TRUE(pool.owns(ptr));

    int stack_var = 0;
    ASSERT_FALSE(pool.owns(&stack_var));

    pool.deallocate(ptr);
}

TEST(write_to_block) {
    ws::MemoryPool<256, 4> pool;
    void* ptr = pool.allocate();

    // Write data into the block
    const char* test_data = "Hello, memory pool!";
    std::memcpy(ptr, test_data, std::strlen(test_data) + 1);

    // Verify data is intact
    ASSERT_EQ(std::strcmp(static_cast<char*>(ptr), test_data), 0);

    pool.deallocate(ptr);
}

TEST(pool_ptr_raii) {
    ws::MemoryPool<64, 4> pool;

    {
        ws::PoolPtr<decltype(pool)> ptr(pool);
        ASSERT_TRUE(static_cast<bool>(ptr));
        ASSERT_EQ(pool.free_count(), 3u);
    }
    // After PoolPtr goes out of scope, block should be returned
    ASSERT_EQ(pool.free_count(), 4u);
}

TEST(pool_ptr_move) {
    ws::MemoryPool<64, 4> pool;

    ws::PoolPtr<decltype(pool)> ptr1(pool);
    ASSERT_EQ(pool.free_count(), 3u);

    ws::PoolPtr<decltype(pool)> ptr2(std::move(ptr1));
    ASSERT_EQ(pool.free_count(), 3u);  // Still 3, ownership transferred

    ASSERT_FALSE(static_cast<bool>(ptr1));  // ptr1 is null after move
    ASSERT_TRUE(static_cast<bool>(ptr2));   // ptr2 is valid
}

TEST(pool_ptr_release) {
    ws::MemoryPool<64, 4> pool;

    void* raw = nullptr;
    {
        ws::PoolPtr<decltype(pool)> ptr(pool);
        raw = ptr.release();
        ASSERT_NE(raw, nullptr);
    }
    // After release + PoolPtr destruction, block is NOT returned
    ASSERT_EQ(pool.free_count(), 3u);

    // Manual cleanup
    pool.deallocate(raw);
    ASSERT_EQ(pool.free_count(), 4u);
}

TEST(full_cycle_exhaust_refill) {
    ws::MemoryPool<128, 8> pool;

    // Run multiple exhaust-refill cycles
    for (int cycle = 0; cycle < 5; ++cycle) {
        std::vector<void*> ptrs;
        for (int i = 0; i < 8; ++i) {
            void* p = pool.allocate();
            ASSERT_NE(p, nullptr);
            ptrs.push_back(p);
        }
        ASSERT_EQ(pool.free_count(), 0u);
        ASSERT_EQ(pool.allocate(), nullptr);

        for (auto* p : ptrs) {
            pool.deallocate(p);
        }
        ASSERT_EQ(pool.free_count(), 8u);
    }
}

TEST(null_deallocate_is_safe) {
    ws::MemoryPool<64, 4> pool;
    pool.deallocate(nullptr);  // Should not crash
    ASSERT_EQ(pool.free_count(), 4u);
}

// ─────────────────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────────────────

int main() {
    std::cout << "\n=== Memory Pool Tests ===\n\n";

    std::cout << "\n";
    std::cout << "Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n\n";

    return tests_failed > 0 ? 1 : 0;
}
