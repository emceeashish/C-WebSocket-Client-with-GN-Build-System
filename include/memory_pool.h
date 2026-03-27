#pragma once

/// @file memory_pool.h
/// @brief Fixed-size block memory pool for zero-allocation hot paths.
///
/// Eliminates heap allocation during WebSocket message processing
/// by pre-allocating a pool of fixed-size buffers at startup.
///
/// Design decisions:
///   - Fixed block size avoids fragmentation
///   - Free-list implemented via embedded pointers (no separate metadata)
///   - Cache-line aligned blocks for optimal memory access patterns
///   - Thread-unsafe by design (each thread gets its own pool)

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace ws {

/// @brief Fixed-size block memory pool.
/// @tparam BlockSize Size of each block in bytes (must be >= sizeof(void*))
/// @tparam NumBlocks Number of pre-allocated blocks
///
/// Memory layout:
///   [Block0][Block1][Block2]...[BlockN-1]
///   Each free block's first bytes store a pointer to the next free block.
///
/// Allocation: O(1) — pop from free list head
/// Deallocation: O(1) — push to free list head
template <std::size_t BlockSize, std::size_t NumBlocks>
class MemoryPool {
    static_assert(BlockSize >= sizeof(void*),
                  "BlockSize must be at least sizeof(void*)");
    static_assert(NumBlocks > 0, "NumBlocks must be positive");

    // Align block size to cache line for better access patterns
    static constexpr std::size_t kAlignedBlockSize =
        (BlockSize + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);

public:
    MemoryPool() {
        // Allocate contiguous storage
        storage_ = std::make_unique<uint8_t[]>(kAlignedBlockSize * NumBlocks);

        // Build the free list by linking all blocks together
        free_head_ = nullptr;
        for (std::size_t i = NumBlocks; i > 0; --i) {
            auto* block = storage_.get() + (i - 1) * kAlignedBlockSize;
            *reinterpret_cast<void**>(block) = free_head_;
            free_head_ = block;
        }

        free_count_ = NumBlocks;
    }

    // Non-copyable, non-movable
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    /// @brief Allocate a block from the pool.
    /// @return Pointer to a block of BlockSize bytes, or nullptr if pool is exhausted.
    ///
    /// O(1) — simply pops from the free list head. No syscall, no lock.
    [[nodiscard]] void* allocate() noexcept {
        if (!free_head_) {
            return nullptr;  // Pool exhausted
        }

        void* block = free_head_;
        free_head_ = *reinterpret_cast<void**>(free_head_);
        --free_count_;
        return block;
    }

    /// @brief Return a block to the pool.
    /// @param ptr Pointer previously obtained from allocate().
    ///
    /// O(1) — pushes to the free list head. No syscall, no lock.
    void deallocate(void* ptr) noexcept {
        if (!ptr) return;

        assert(owns(ptr) && "Pointer does not belong to this pool");

        *reinterpret_cast<void**>(ptr) = free_head_;
        free_head_ = ptr;
        ++free_count_;
    }

    /// @brief Check if a pointer belongs to this pool.
    [[nodiscard]] bool owns(void* ptr) const noexcept {
        auto* p = static_cast<uint8_t*>(ptr);
        return p >= storage_.get() &&
               p < storage_.get() + kAlignedBlockSize * NumBlocks;
    }

    /// @brief Number of free blocks remaining.
    [[nodiscard]] std::size_t free_count() const noexcept { return free_count_; }

    /// @brief Total pool capacity.
    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return NumBlocks; }

    /// @brief Size of each block.
    [[nodiscard]] constexpr std::size_t block_size() const noexcept { return BlockSize; }

private:
    std::unique_ptr<uint8_t[]> storage_;
    void*       free_head_  = nullptr;
    std::size_t free_count_ = 0;
};

/// @brief RAII wrapper for pool-allocated memory.
/// @tparam Pool The MemoryPool type
///
/// Automatically returns memory to the pool on destruction.
/// Usage:
///   PoolPtr<decltype(pool)> ptr(pool);
///   std::memcpy(ptr.get(), data, size);
template <typename Pool>
class PoolPtr {
public:
    explicit PoolPtr(Pool& pool) : pool_(pool), ptr_(pool.allocate()) {}

    ~PoolPtr() {
        if (ptr_) {
            pool_.deallocate(ptr_);
        }
    }

    // Move-only
    PoolPtr(PoolPtr&& other) noexcept : pool_(other.pool_), ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    PoolPtr& operator=(PoolPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_) pool_.deallocate(ptr_);
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    PoolPtr(const PoolPtr&) = delete;
    PoolPtr& operator=(const PoolPtr&) = delete;

    [[nodiscard]] void* get() noexcept { return ptr_; }
    [[nodiscard]] const void* get() const noexcept { return ptr_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

    /// @brief Release ownership without deallocating.
    [[nodiscard]] void* release() noexcept {
        void* p = ptr_;
        ptr_ = nullptr;
        return p;
    }

private:
    Pool& pool_;
    void* ptr_;
};

}  // namespace ws
