#pragma once

/// @file latency_stats.h
/// @brief High-resolution latency measurement and statistics.
///
/// Captures per-message round-trip times and computes distribution
/// statistics: min, max, mean, median, p95, p99, p99.9.
///
/// Design decisions:
///   - Uses steady_clock for monotonic, high-resolution timing
///   - Stores all samples for accurate percentile computation
///   - Pre-reserves sample vector to avoid reallocation on hot path

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace ws {

/// @brief High-resolution clock wrapper for latency measurement.
/// Uses std::chrono::steady_clock for monotonic timing.
class LatencyStats {
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration  = std::chrono::nanoseconds;

    /// @brief Construct with pre-allocated sample storage.
    /// @param reserve_size Number of samples to pre-allocate (avoids hot-path realloc)
    explicit LatencyStats(std::size_t reserve_size = 10000) {
        samples_ns_.reserve(reserve_size);
    }

    /// @brief Capture a timestamp (call before send).
    [[nodiscard]] static TimePoint now() noexcept {
        return Clock::now();
    }

    /// @brief Record a latency sample from start to now.
    void record(TimePoint start) {
        auto end = Clock::now();
        auto ns = std::chrono::duration_cast<Duration>(end - start).count();
        samples_ns_.push_back(ns);
    }

    /// @brief Record a pre-computed latency in nanoseconds.
    void record_ns(int64_t nanoseconds) {
        samples_ns_.push_back(nanoseconds);
    }

    /// @brief Get number of recorded samples.
    [[nodiscard]] std::size_t count() const noexcept {
        return samples_ns_.size();
    }

    /// @brief Reset all recorded samples.
    void reset() {
        samples_ns_.clear();
    }

    /// @brief Compute and print latency distribution to stdout.
    void print_report(const std::string& label = "Latency Report") const {
        if (samples_ns_.empty()) {
            std::cout << "[" << label << "] No samples recorded.\n";
            return;
        }

        // Sort a copy for percentile computation
        auto sorted = samples_ns_;
        std::sort(sorted.begin(), sorted.end());

        const auto n = sorted.size();
        const double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);

        const double min_us  = sorted.front() / 1000.0;
        const double max_us  = sorted.back()  / 1000.0;
        const double mean_us = (sum / static_cast<double>(n)) / 1000.0;
        const double med_us  = percentile(sorted, 50.0) / 1000.0;
        const double p95_us  = percentile(sorted, 95.0) / 1000.0;
        const double p99_us  = percentile(sorted, 99.0) / 1000.0;
        const double p999_us = percentile(sorted, 99.9) / 1000.0;

        // Compute standard deviation
        double sq_sum = 0.0;
        for (auto s : sorted) {
            double diff = (s / 1000.0) - mean_us;
            sq_sum += diff * diff;
        }
        const double stddev_us = std::sqrt(sq_sum / static_cast<double>(n));

        std::cout << "\n";
        std::cout << "┌─────────────────────────────────────────────┐\n";
        std::cout << "│ " << std::left << std::setw(43) << label << " │\n";
        std::cout << "├─────────────────────────────────────────────┤\n";
        std::cout << "│  Samples : " << std::setw(32) << n            << "│\n";
        std::cout << "│  Min     : " << std::setw(28) << std::fixed
                  << std::setprecision(2) << min_us  << " µs │\n";
        std::cout << "│  Max     : " << std::setw(28) << max_us  << " µs │\n";
        std::cout << "│  Mean    : " << std::setw(28) << mean_us << " µs │\n";
        std::cout << "│  StdDev  : " << std::setw(28) << stddev_us << " µs │\n";
        std::cout << "│  Median  : " << std::setw(28) << med_us  << " µs │\n";
        std::cout << "│  p95     : " << std::setw(28) << p95_us  << " µs │\n";
        std::cout << "│  p99     : " << std::setw(28) << p99_us  << " µs │\n";
        std::cout << "│  p99.9   : " << std::setw(28) << p999_us << " µs │\n";
        std::cout << "└─────────────────────────────────────────────┘\n";
        std::cout << "\n";
    }

    /// @brief Get raw samples (for external analysis / export).
    [[nodiscard]] const std::vector<int64_t>& samples() const noexcept {
        return samples_ns_;
    }

private:
    /// @brief Compute a percentile from sorted data.
    static double percentile(const std::vector<int64_t>& sorted, double pct) {
        if (sorted.empty()) return 0.0;
        double rank = (pct / 100.0) * static_cast<double>(sorted.size() - 1);
        auto lower = static_cast<std::size_t>(rank);
        auto upper = lower + 1;
        if (upper >= sorted.size()) return static_cast<double>(sorted.back());
        double frac = rank - static_cast<double>(lower);
        return static_cast<double>(sorted[lower]) * (1.0 - frac) +
               static_cast<double>(sorted[upper]) * frac;
    }

    std::vector<int64_t> samples_ns_;  ///< All recorded latencies in nanoseconds
};

}  // namespace ws
