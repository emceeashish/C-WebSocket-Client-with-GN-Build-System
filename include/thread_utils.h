#pragma once

/// @file thread_utils.h
/// @brief CPU affinity and thread pinning utilities.
///
/// In low latency systems, pinning threads to specific CPU cores
/// eliminates context-switch jitter and improves cache locality.
///
/// Design decisions:
///   - Platform-specific implementations (Windows + Linux)
///   - Best-effort: returns success/failure, never throws
///   - Thread naming for debugger visibility (Linux: prctl, Windows: SetThreadDescription)

#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
    #include <sched.h>
    #include <sys/prctl.h>
#endif

namespace ws {

/// @brief Pin the calling thread to a specific CPU core.
/// @param core_id Zero-based CPU core index.
/// @return true if pinning succeeded, false otherwise.
///
/// Why pin threads?
///   - Eliminates OS scheduler migrations between cores
///   - Keeps the L1/L2 cache warm for this thread's data
///   - Reduces tail latency caused by cache-cold restarts
///   - Critical for achieving consistent sub-microsecond latency
inline bool pin_thread_to_core(int core_id) noexcept {
#ifdef _WIN32
    DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core_id;
    HANDLE thread = GetCurrentThread();
    DWORD_PTR result = SetThreadAffinityMask(thread, mask);
    if (result == 0) {
        std::cerr << "[thread] Failed to pin to core " << core_id
                  << " (error: " << GetLastError() << ")\n";
        return false;
    }

    // Also set high priority to reduce scheduling latency
    if (!SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST)) {
        std::cerr << "[thread] Warning: could not set high priority\n";
    }

    return true;
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "[thread] Failed to pin to core " << core_id
                  << " (errno: " << rc << ")\n";
        return false;
    }

    return true;
#endif
}

/// @brief Set the name of the calling thread (visible in debuggers/profilers).
/// @param name Thread name (max 15 chars on Linux).
inline void set_thread_name(const std::string& name) noexcept {
#ifdef _WIN32
    // Windows 10 1607+ thread naming
    std::wstring wname(name.begin(), name.end());
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (hKernel) {
        // Two-step cast via void* to avoid -Wcast-function-type
        auto raw = reinterpret_cast<void*>(
            GetProcAddress(hKernel, "SetThreadDescription"));
        auto fn = reinterpret_cast<SetThreadDescriptionFn>(raw);
        if (fn) {
            fn(GetCurrentThread(), wname.c_str());
        }
    }
#else
    // Linux: thread name limited to 15 chars + null
    prctl(PR_SET_NAME, name.substr(0, 15).c_str(), 0, 0, 0);
#endif
}

/// @brief Query the number of available CPU cores.
/// @return Number of hardware threads, or 1 if detection fails.
inline unsigned int get_cpu_count() noexcept {
    unsigned int count = std::thread::hardware_concurrency();
    return count > 0 ? count : 1;
}

/// @brief RAII thread pinner — pins on construction, logs on failure.
///
/// Usage:
///   void my_thread_func() {
///       ThreadPinner pinner(2, "receiver");
///       // ... thread is now pinned to core 2
///   }
class ThreadPinner {
public:
    /// @param core_id CPU core to pin to
    /// @param name Optional thread name for debugger
    explicit ThreadPinner(int core_id, const std::string& name = "")
        : core_id_(core_id), pinned_(false)
    {
        if (!name.empty()) {
            set_thread_name(name);
        }

        pinned_ = pin_thread_to_core(core_id);

        if (pinned_) {
            std::cout << "[thread] '" << name << "' pinned to core "
                      << core_id << " / " << get_cpu_count() << " cores\n";
        }
    }

    [[nodiscard]] bool is_pinned() const noexcept { return pinned_; }
    [[nodiscard]] int core_id() const noexcept { return core_id_; }

private:
    int  core_id_;
    bool pinned_;
};

}  // namespace ws
