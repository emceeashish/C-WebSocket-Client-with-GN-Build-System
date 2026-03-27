#pragma once

// Lock-free SPSC ring buffer for inter-thread message passing.
// Cache-line aligned head/tail to prevent false sharing.
// Power-of-two capacity for branchless modulo (bitwise AND).

#include <atomic>
#include <array>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace ws {

inline constexpr std::size_t kCacheLineSize = 64;

template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_move_constructible_v<T>,
                  "T must be move constructible");

public:
    SPSCQueue() : head_(0), tail_(0) {}

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) = delete;
    SPSCQueue& operator=(SPSCQueue&&) = delete;

    // Producer side. Relaxed load for tail (only we write it),
    // acquire for head (consumer may have advanced it).
    template <typename U>
    bool try_push(U&& item) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto next_tail = (tail + 1) & kMask;

        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[tail] = std::forward<U>(item);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Consumer side. Relaxed load for head (only we write it),
    // acquire for tail (producer may have advanced it).
    std::optional<T> try_pop() noexcept {
        const auto head = head_.load(std::memory_order_relaxed);

        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        T item = std::move(buffer_[head]);
        head_.store((head + 1) & kMask, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] constexpr std::size_t capacity() const noexcept {
        return Capacity;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto head = head_.load(std::memory_order_acquire);
        return (tail - head) & kMask;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Separate cache lines to prevent false sharing
    alignas(kCacheLineSize) std::atomic<std::size_t> head_;
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_;

    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};

}  // namespace ws
