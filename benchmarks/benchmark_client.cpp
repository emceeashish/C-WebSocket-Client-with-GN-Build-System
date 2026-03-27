/// @file benchmark_client.cpp
/// @brief Dedicated latency benchmarking suite.
///
/// Runs multiple benchmark scenarios and reports comprehensive
/// latency distribution for each. Designed to be run independently
/// of the interactive client.
///
/// Scenarios:
///   1. Throughput benchmark (burst N messages, measure total time)
///   2. Latency benchmark (sequential send-receive, per-message RTT)
///   3. Warm-up effect analysis (compare cold vs. warm latency)
///   4. Message size impact (vary payload size, measure RTT)

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "latency_stats.h"
#include "websocket_client.h"

namespace net = boost::asio;

/// @brief Generate a payload of specified size.
std::string make_payload(std::size_t size) {
    std::string payload;
    payload.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        payload += static_cast<char>('A' + (i % 26));
    }
    return payload;
}

/// @brief Print a section header.
void print_header(const std::string& title) {
    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════\n";
    std::cout << "  " << title << "\n";
    std::cout << "════════════════════════════════════════════════\n";
}

/// @brief Benchmark 1: Sequential round-trip latency.
///
/// Sends messages one at a time, waiting for each response
/// before sending the next. Measures true RTT per message.
void benchmark_sequential_rtt(ws::WebSocketClient& client, int num_messages) {
    print_header("Sequential RTT Benchmark (" + std::to_string(num_messages) + " msgs)");

    ws::LatencyStats stats(num_messages);

    for (int i = 0; i < num_messages; ++i) {
        std::string msg = "seq_" + std::to_string(i);

        auto t_start = ws::LatencyStats::now();
        client.send_text(msg);
        auto response = client.spin_receive();
        stats.record(t_start);
    }

    stats.print_report("Sequential RTT");
}

/// @brief Benchmark 2: Throughput (burst mode).
///
/// Sends all messages as fast as possible, then drains responses.
/// Measures total throughput in messages/second.
void benchmark_throughput(ws::WebSocketClient& client, int num_messages) {
    print_header("Throughput Benchmark (" + std::to_string(num_messages) + " msgs)");

    auto t_start = std::chrono::steady_clock::now();

    // Burst send
    for (int i = 0; i < num_messages; ++i) {
        std::string msg = "burst_" + std::to_string(i);
        client.send_text(msg);
    }

    // Drain all responses
    int received = 0;
    while (received < num_messages) {
        if (auto msg = client.try_receive()) {
            ++received;
        }
    }

    auto t_end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          t_end - t_start).count();
    auto elapsed_ms = elapsed_us / 1000.0;
    auto msgs_per_sec = (num_messages / (elapsed_ms / 1000.0));

    std::cout << "\n";
    std::cout << "┌─────────────────────────────────────────────┐\n";
    std::cout << "│ Throughput Results" << std::string(26, ' ') << " │\n";
    std::cout << "├─────────────────────────────────────────────┤\n";
    std::cout << "│  Messages     : " << std::left << std::setw(28) << num_messages << "│\n";
    std::cout << "│  Total time   : " << std::setw(24) << std::fixed << std::setprecision(2)
              << elapsed_ms << " ms │\n";
    std::cout << "│  Throughput   : " << std::setw(22) << std::setprecision(0)
              << msgs_per_sec << " msg/s │\n";
    std::cout << "│  Avg interval : " << std::setw(24) << std::setprecision(2)
              << (elapsed_us / static_cast<double>(num_messages)) << " µs │\n";
    std::cout << "└─────────────────────────────────────────────┘\n";
}

/// @brief Benchmark 3: Warm-up effect analysis.
///
/// Compares latency of first N messages (cold) vs. subsequent
/// messages (warm). Demonstrates cache warming and JIT effects.
void benchmark_warmup_effect(ws::WebSocketClient& client,
                             int warmup_count, int measure_count) {
    print_header("Warm-Up Effect Analysis");

    // Cold phase
    ws::LatencyStats cold_stats(warmup_count);
    for (int i = 0; i < warmup_count; ++i) {
        std::string msg = "cold_" + std::to_string(i);
        auto t = ws::LatencyStats::now();
        client.send_text(msg);
        client.spin_receive();
        cold_stats.record(t);
    }

    // Warm phase
    ws::LatencyStats warm_stats(measure_count);
    for (int i = 0; i < measure_count; ++i) {
        std::string msg = "warm_" + std::to_string(i);
        auto t = ws::LatencyStats::now();
        client.send_text(msg);
        client.spin_receive();
        warm_stats.record(t);
    }

    cold_stats.print_report("Cold Start (" + std::to_string(warmup_count) + " msgs)");
    warm_stats.print_report("Warm State (" + std::to_string(measure_count) + " msgs)");
}

/// @brief Benchmark 4: Message size impact.
///
/// Varies payload size and measures RTT for each size.
/// Shows the relationship between message size and latency.
void benchmark_message_sizes(ws::WebSocketClient& client) {
    print_header("Message Size Impact");

    std::vector<std::size_t> sizes = {16, 64, 256, 1024, 4096, 16384};
    constexpr int kMessagesPerSize = 50;

    std::cout << "\n";
    std::cout << "┌─────────┬──────────┬──────────┬──────────┬──────────┐\n";
    std::cout << "│  Size   │  Min µs  │  Mean µs │  p99 µs  │  Max µs  │\n";
    std::cout << "├─────────┼──────────┼──────────┼──────────┼──────────┤\n";

    for (auto size : sizes) {
        std::string payload = make_payload(size);
        ws::LatencyStats stats(kMessagesPerSize);

        for (int i = 0; i < kMessagesPerSize; ++i) {
            auto t = ws::LatencyStats::now();
            client.send_text(payload);
            client.spin_receive();
            stats.record(t);
        }

        auto& samples = stats.samples();
        auto sorted = samples;
        std::sort(sorted.begin(), sorted.end());

        double min_us  = sorted.front() / 1000.0;
        double max_us  = sorted.back() / 1000.0;
        double sum = 0;
        for (auto s : sorted) sum += s;
        double mean_us = (sum / sorted.size()) / 1000.0;

        // p99
        auto p99_idx = static_cast<size_t>(0.99 * (sorted.size() - 1));
        double p99_us = sorted[p99_idx] / 1000.0;

        std::cout << "│ " << std::right << std::setw(5) << size << " B"
                  << " │ " << std::setw(8) << std::fixed << std::setprecision(1) << min_us
                  << " │ " << std::setw(8) << mean_us
                  << " │ " << std::setw(8) << p99_us
                  << " │ " << std::setw(8) << max_us
                  << " │\n";
    }

    std::cout << "└─────────┴──────────┴──────────┴──────────┴──────────┘\n";
}

int main(int argc, char* argv[]) {
    std::string host = ws::config::kDefaultHost;
    std::string port = ws::config::kDefaultPort;

    if (argc >= 3) {
        host = argv[1];
        port = argv[2];
    }

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════╗\n";
    std::cout << "║     WebSocket Latency Benchmark Suite        ║\n";
    std::cout << "╠══════════════════════════════════════════════╣\n";
    std::cout << "║  Server: " << std::left << std::setw(35) << (host + ":" + port) << " ║\n";
    std::cout << "║  Queue:  " << std::setw(35) << (std::to_string(ws::kQueueCapacity) + " slots (SPSC lock-free)") << " ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n";

    try {
        net::io_context ioc;
        ws::WebSocketClient client(ioc, host, port);

        std::cout << "\n[*] Connecting to " << host << ":" << port << "...\n";
        client.connect();

        // Consume welcome message
        for (int i = 0; i < 50; ++i) {
            if (client.try_receive()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Run all benchmarks
        benchmark_sequential_rtt(client, ws::config::kBenchmarkMessages);
        benchmark_throughput(client, ws::config::kBenchmarkMessages);
        benchmark_warmup_effect(client, ws::config::kWarmupMessages,
                                ws::config::kBenchmarkMessages);
        benchmark_message_sizes(client);

        std::cout << "\n[✓] All benchmarks complete.\n\n";
        client.close();

    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
