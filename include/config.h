#pragma once

// Compile-time configuration — all tunables are constexpr (zero overhead).
// Template policies allow swapping wait strategies without virtual dispatch.

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

// ── Network ──────────────────────────────────────────────

inline constexpr const char* kDefaultHost = "echo.websocket.events";
inline constexpr const char* kDefaultPort = "80";

inline constexpr bool   kTcpNoDelay       = true;
inline constexpr size_t kSendBufferSize   = 65536;
inline constexpr size_t kRecvBufferSize   = 65536;

// ── Queue ────────────────────────────────────────────────

inline constexpr size_t kQueueCapacity = 1024;

template <size_t N>
struct IsPowerOfTwo : std::bool_constant<(N > 0) && ((N & (N - 1)) == 0)> {};

static_assert(IsPowerOfTwo<kQueueCapacity>::value,
              "Queue capacity must be a power of two");

// ── Memory ───────────────────────────────────────────────

inline constexpr size_t kCacheLineSize = 64;

inline constexpr size_t kPoolBlockSize = 4096;
inline constexpr size_t kPoolNumBlocks = 256;
inline constexpr size_t kPoolTotalBytes = kPoolBlockSize * kPoolNumBlocks;
inline constexpr size_t kPoolTotalKB    = kPoolTotalBytes / 1024;
inline constexpr size_t kReadBufferSize = 65536;

// ── Latency ──────────────────────────────────────────────

inline constexpr size_t kDefaultSampleReserve = 10000;
inline constexpr int kBenchmarkMessages = 100;
inline constexpr int kWarmupMessages    = 10;

// ── Reconnection ─────────────────────────────────────────

inline constexpr uint32_t kInitialBackoffMs  = 100;
inline constexpr uint32_t kMaxBackoffMs      = 30000;
inline constexpr double   kBackoffMultiplier = 2.0;
inline constexpr int      kMaxReconnectTries = 10;

// ── Heartbeat ────────────────────────────────────────────

inline constexpr uint32_t kHeartbeatIntervalS = 30;
inline constexpr uint32_t kHeartbeatTimeoutS  = 10;

// ── Wait Policies (compile-time swap, zero cost) ─────────

// Spin-wait: uses _mm_pause() on x86, yield on ARM
struct SpinWaitPolicy {
    static void wait() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }

    static constexpr const char* name() noexcept { return "spin-wait"; }
};

// Yield: gives up timeslice to OS scheduler
struct YieldWaitPolicy {
    static void wait() noexcept {
        std::this_thread::yield();
    }

    static constexpr const char* name() noexcept { return "yield"; }
};

// Sleep: lowest CPU, highest latency
template <uint32_t MicrosecondsSleep = 1>
struct SleepWaitPolicy {
    static void wait() noexcept {
        std::this_thread::sleep_for(
            std::chrono::microseconds(MicrosecondsSleep));
    }

    static constexpr const char* name() noexcept { return "sleep"; }
};

template <bool LowLatency>
using SelectWaitPolicy = std::conditional_t<LowLatency, SpinWaitPolicy, YieldWaitPolicy>;

// ── Feature Flags ────────────────────────────────────────

inline constexpr bool kEnableLatencyTracking = true;
inline constexpr bool kEnableMemoryPool      = true;
inline constexpr bool kEnableThreadPinning   = true;
inline constexpr bool kEnableHeartbeat       = true;

template <bool Enabled>
struct Feature {
    static constexpr bool enabled = Enabled;
};

}  // namespace config
}  // namespace ws
