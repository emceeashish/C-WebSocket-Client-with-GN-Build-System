#pragma once

// Fixed-size block memory pool for zero-allocation hot paths.
// Free-list via embedded pointers — O(1) alloc/dealloc, no fragmentation.
// Thread-unsafe by design (each thread gets its own pool).

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace ws {

template <std::size_t BlockSize, std::size_t NumBlocks>
class MemoryPool {
    static_assert(BlockSize >= sizeof(void*),
                  "BlockSize must be at least sizeof(void*)");
    static_assert(NumBlocks > 0, "NumBlocks must be positive");

    static constexpr std::size_t kAlignedBlockSize =
        (BlockSize + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);

public:
    MemoryPool() {
        storage_ = std::make_unique<uint8_t[]>(kAlignedBlockSize * NumBlocks);

        // Build free list by linking all blocks in reverse
        free_head_ = nullptr;
        for (std::size_t i = NumBlocks; i > 0; --i) {
            auto* block = storage_.get() + (i - 1) * kAlignedBlockSize;
            *reinterpret_cast<void**>(block) = free_head_;
            free_head_ = block;
        }

        free_count_ = NumBlocks;
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    // O(1) — pop from free list head
    [[nodiscard]] void* allocate() noexcept {
        if (!free_head_) {
            return nullptr;
        }

        void* block = free_head_;
        free_head_ = *reinterpret_cast<void**>(free_head_);
        --free_count_;
        return block;
    }

    // O(1) — push to free list head
    void deallocate(void* ptr) noexcept {
        if (!ptr) return;

        assert(owns(ptr) && "Pointer does not belong to this pool");

        *reinterpret_cast<void**>(ptr) = free_head_;
        free_head_ = ptr;
        ++free_count_;
    }

    [[nodiscard]] bool owns(void* ptr) const noexcept {
        auto* p = static_cast<uint8_t*>(ptr);
        return p >= storage_.get() &&
               p < storage_.get() + kAlignedBlockSize * NumBlocks;
    }

    [[nodiscard]] std::size_t free_count() const noexcept { return free_count_; }
    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return NumBlocks; }
    [[nodiscard]] constexpr std::size_t block_size() const noexcept { return BlockSize; }

private:
    std::unique_ptr<uint8_t[]> storage_;
    void*       free_head_  = nullptr;
    std::size_t free_count_ = 0;
};

// RAII wrapper — auto-returns memory to pool on destruction
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
