#pragma once

/// @file config.h
/// @brief Compile-time configuration using constexpr and template metaprogramming.
///
/// All tunable parameters are constexpr, enabling the compiler to
/// optimize them away at compile time (zero-cost abstraction).
/// Template policies allow swapping behavior without virtual dispatch.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>

// x86 intrinsics for _mm_pause() spin-wait hint
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
#endif

namespace ws {
namespace config {

// ─────────────────────────────────────────────────────────
//  Network Configuration
// ─────────────────────────────────────────────────────────

/// Default server to connect to
inline constexpr const char* kDefaultHost = "echo.websocket.events";
inline constexpr const char* kDefaultPort = "80";

/// TCP socket options
inline constexpr bool   kTcpNoDelay       = true;   ///< Disable Nagle's algorithm
inline constexpr size_t kSendBufferSize   = 65536;   ///< Socket send buffer (bytes)
inline constexpr size_t kRecvBufferSize   = 65536;   ///< Socket receive buffer (bytes)

// ─────────────────────────────────────────────────────────
//  Queue Configuration
// ─────────────────────────────────────────────────────────

/// SPSC queue capacity (must be power of two)
inline constexpr size_t kQueueCapacity = 1024;

/// Compile-time check for power of two
template <size_t N>
struct IsPowerOfTwo : std::bool_constant<(N > 0) && ((N & (N - 1)) == 0)> {};

static_assert(IsPowerOfTwo<kQueueCapacity>::value,
              "Queue capacity must be a power of two");

// ─────────────────────────────────────────────────────────
//  Memory Configuration
// ─────────────────────────────────────────────────────────

/// Cache line size (x86_64 = 64 bytes, ARM = 64 or 128)
inline constexpr size_t kCacheLineSize = 64;

/// Memory pool parameters
inline constexpr size_t kPoolBlockSize = 4096;   ///< Bytes per block
inline constexpr size_t kPoolNumBlocks = 256;    ///< Number of pre-allocated blocks

/// Compile-time total pool size calculation
inline constexpr size_t kPoolTotalBytes = kPoolBlockSize * kPoolNumBlocks;
inline constexpr size_t kPoolTotalKB    = kPoolTotalBytes / 1024;

/// Read buffer size (pre-allocated flat_buffer)
inline constexpr size_t kReadBufferSize = 65536;

// ─────────────────────────────────────────────────────────
//  Latency Configuration
// ─────────────────────────────────────────────────────────

/// Default number of samples to pre-allocate in LatencyStats
inline constexpr size_t kDefaultSampleReserve = 10000;

/// Benchmark configuration
inline constexpr int kBenchmarkMessages = 100;
inline constexpr int kWarmupMessages    = 10;    ///< Warm up before measuring

// ─────────────────────────────────────────────────────────
//  Reconnection Configuration
// ─────────────────────────────────────────────────────────

inline constexpr uint32_t kInitialBackoffMs  = 100;
inline constexpr uint32_t kMaxBackoffMs      = 30000;
inline constexpr double   kBackoffMultiplier = 2.0;
inline constexpr int      kMaxReconnectTries = 10;

// ─────────────────────────────────────────────────────────
//  Heartbeat Configuration
// ─────────────────────────────────────────────────────────

inline constexpr uint32_t kHeartbeatIntervalS = 30;
inline constexpr uint32_t kHeartbeatTimeoutS  = 10;

// ─────────────────────────────────────────────────────────
//  Template Policies (Zero-Cost Abstractions)
// ─────────────────────────────────────────────────────────

/// @brief Spin-wait policy — uses _mm_pause() on x86.
/// Can be swapped at compile time for different wait strategies.
struct SpinWaitPolicy {
    static void wait() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        _mm_pause();
#else
        // ARM: yield instruction or busy spin
        std::this_thread::yield();
#endif
    }

    static constexpr const char* name() noexcept { return "spin-wait"; }
};

/// @brief Yield policy — yields to OS scheduler.
/// Lower CPU usage, higher latency.
struct YieldWaitPolicy {
    static void wait() noexcept {
        std::this_thread::yield();
    }

    static constexpr const char* name() noexcept { return "yield"; }
};

/// @brief Sleep policy — sleeps for a fixed duration.
/// Lowest CPU usage, highest latency.
template <uint32_t MicrosecondsSleep = 1>
struct SleepWaitPolicy {
    static void wait() noexcept {
        std::this_thread::sleep_for(
            std::chrono::microseconds(MicrosecondsSleep));
    }

    static constexpr const char* name() noexcept { return "sleep"; }
};

/// @brief Compile-time selection of wait policy based on latency requirements.
/// Usage: using WaitPolicy = SelectWaitPolicy<true>;  // spin if low latency
template <bool LowLatency>
using SelectWaitPolicy = std::conditional_t<LowLatency, SpinWaitPolicy, YieldWaitPolicy>;

// ─────────────────────────────────────────────────────────
//  Compile-Time Feature Flags
// ─────────────────────────────────────────────────────────

/// Enable/disable features at compile time (zero overhead when disabled)
inline constexpr bool kEnableLatencyTracking = true;
inline constexpr bool kEnableMemoryPool      = true;
inline constexpr bool kEnableThreadPinning   = true;
inline constexpr bool kEnableHeartbeat       = true;

/// @brief Compile-time feature check.
/// Usage: if constexpr (Feature<kEnableLatencyTracking>::enabled) { ... }
template <bool Enabled>
struct Feature {
    static constexpr bool enabled = Enabled;
};

}  // namespace config
}  // namespace ws
