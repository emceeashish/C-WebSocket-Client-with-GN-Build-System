#pragma once

/// @file spsc_queue.h
/// @brief Lock-free Single Producer Single Consumer (SPSC) bounded ring buffer.
///
/// This is a cache-friendly, lock-free queue designed for low latency
/// inter-thread communication. It uses acquire-release memory ordering
/// to ensure correctness without mutexes or condition variables.
///
/// Design decisions:
///   - Cache-line aligned head/tail to prevent false sharing
///   - Power-of-two capacity for branchless modulo (bitwise AND)
///   - Pre-allocated contiguous storage to avoid heap allocations on hot path
///   - Relaxed loads for local index, acquire/release for shared index

#include <atomic>
#include <array>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace ws {

/// Cache line size for alignment — prevents false sharing between producer/consumer
inline constexpr std::size_t kCacheLineSize = 64;

/// @brief Lock-free SPSC bounded ring buffer.
/// @tparam T Element type (must be move-constructible)
/// @tparam Capacity Must be a power of two for branchless modulo
///
/// Memory layout:
///   [head_ (cache line 0)] [tail_ (cache line 1)] [buffer_ (contiguous)]
///
/// The producer writes to tail_, the consumer reads from head_.
/// Both indices only ever increase (modular arithmetic via mask).
template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_move_constructible_v<T>,
                  "T must be move constructible");

public:
    SPSCQueue() : head_(0), tail_(0) {}

    // Non-copyable, non-movable (atomic members)
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) = delete;
    SPSCQueue& operator=(SPSCQueue&&) = delete;

    /// @brief Try to push an element (producer side).
    /// @return true if element was enqueued, false if queue is full.
    ///
    /// Uses relaxed load for tail_ (only producer writes to it),
    /// and acquire load for head_ (consumer may have advanced it).
    template <typename U>
    bool try_push(U&& item) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto next_tail = (tail + 1) & kMask;

        // Check if queue is full
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // Queue full — caller decides what to do
        }

        buffer_[tail] = std::forward<U>(item);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    /// @brief Try to pop an element (consumer side).
    /// @return The element if available, std::nullopt if queue is empty.
    ///
    /// Uses relaxed load for head_ (only consumer writes to it),
    /// and acquire load for tail_ (producer may have advanced it).
    std::optional<T> try_pop() noexcept {
        const auto head = head_.load(std::memory_order_relaxed);

        // Check if queue is empty
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;  // Queue empty
        }

        T item = std::move(buffer_[head]);
        head_.store((head + 1) & kMask, std::memory_order_release);
        return item;
    }

    /// @brief Check if the queue is empty (approximate — may be stale).
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    /// @brief Returns the fixed capacity of the queue.
    [[nodiscard]] constexpr std::size_t capacity() const noexcept {
        return Capacity;
    }

    /// @brief Returns approximate number of elements in the queue.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto head = head_.load(std::memory_order_acquire);
        return (tail - head) & kMask;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Cache-line aligned to prevent false sharing between producer and consumer
    alignas(kCacheLineSize) std::atomic<std::size_t> head_;
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_;

    // Pre-allocated contiguous buffer — no heap allocations on hot path
    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};

}  // namespace ws
