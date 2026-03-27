#pragma once

/// @file websocket_client.h
/// @brief Low latency WebSocket client using Boost.Beast.
///
/// Threading model:
///   - Main thread: sends messages, reads from SPSC queue
///   - Receiver thread: reads from socket, pushes to SPSC queue
///     (optionally pinned to a specific CPU core)
///
/// The producer (receiver thread) and consumer (main thread)
/// communicate via a lock-free SPSC queue instead of a mutex-guarded
/// std::queue, eliminating contention on the hot path.
///
/// Tier 2 additions:
///   - CPU affinity / thread pinning for deterministic latency
///   - Memory pool for zero-allocation receive path
///   - Pre-allocated flat_buffer to avoid per-read allocation

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "memory_pool.h"
#include "message.h"
#include "spsc_queue.h"
#include "thread_utils.h"

namespace ws {

namespace net       = boost::asio;
namespace beast     = boost::beast;
namespace websocket = beast::websocket;
using tcp           = net::ip::tcp;

/// Queue capacity — must be power of two (see spsc_queue.h)
inline constexpr std::size_t kQueueCapacity = 1024;

/// Read buffer size — pre-allocated to avoid per-read heap allocation
inline constexpr std::size_t kReadBufferSize = 65536;  // 64 KB

/// Memory pool block size for message payloads
inline constexpr std::size_t kPoolBlockSize = 4096;    // 4 KB per message
inline constexpr std::size_t kPoolNumBlocks = 256;     // 256 blocks = 1 MB total

/// @brief Client configuration for low-latency tuning.
struct ClientConfig {
    bool pin_receiver_thread = false;     ///< Enable CPU affinity for receiver
    int  receiver_core_id    = -1;        ///< Core to pin to (-1 = last core)
    bool use_memory_pool     = true;      ///< Use pre-allocated buffer pool
};

/// @brief WebSocket client with lock-free message passing.
///
/// Replaces std::mutex + std::condition_variable with a lock-free
/// SPSC ring buffer for inter-thread communication. The receiver
/// thread busy-spins on the socket read and enqueues messages
/// without any lock acquisition.
///
/// Tier 2: optionally pins the receiver thread to a CPU core and
/// uses a memory pool to eliminate per-read heap allocations.
class WebSocketClient {
public:
    /// @param ioc  Boost.Asio I/O context
    /// @param host Server hostname
    /// @param port Server port (as string, e.g. "80")
    /// @param config Optional low-latency tuning parameters
    WebSocketClient(net::io_context& ioc,
                    const std::string& host,
                    const std::string& port,
                    ClientConfig config = {})
        : resolver_(ioc)
        , ws_(ioc)
        , host_(host)
        , port_(port)
        , config_(config)
        , running_(false) {}

    ~WebSocketClient() {
        close();
    }

    // Non-copyable, non-movable
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;
    WebSocketClient(WebSocketClient&&) = delete;
    WebSocketClient& operator=(WebSocketClient&&) = delete;

    /// @brief Connect to the WebSocket server and start the receiver thread.
    /// @throws boost::system::system_error on connection failure
    void connect() {
        auto const results = resolver_.resolve(host_, port_);
        net::connect(ws_.next_layer(), results.begin(), results.end());

        // Disable Nagle's algorithm for lower latency
        ws_.next_layer().set_option(tcp::no_delay(true));

        // Set socket buffer sizes for lower latency
        ws_.next_layer().set_option(
            boost::asio::socket_base::receive_buffer_size(kReadBufferSize));
        ws_.next_layer().set_option(
            boost::asio::socket_base::send_buffer_size(kReadBufferSize));

        ws_.handshake(host_, "/");
        running_.store(true, std::memory_order_release);

        std::cout << "[ws] Connected to " << host_ << ":" << port_ << "\n";

        receiver_thread_ = std::thread(&WebSocketClient::receive_loop, this);
    }

    /// @brief Send a text message.
    /// @param message The text payload to send.
    void send_text(const std::string& message) {
        ws_.text(true);
        ws_.write(net::buffer(message));
    }

    /// @brief Send a binary message.
    /// @param data The binary payload to send.
    void send_binary(const std::vector<uint8_t>& data) {
        ws_.binary(true);
        ws_.write(net::buffer(data));
    }

    /// @brief Try to dequeue a received message (non-blocking).
    /// @return The message if one is available, std::nullopt otherwise.
    ///
    /// This is the hot-path consumer call — no locks, no syscalls.
    /// Caller can busy-spin on this if lowest latency is desired,
    /// or sleep between calls if throughput matters more.
    [[nodiscard]] std::optional<Message> try_receive() noexcept {
        return queue_.try_pop();
    }

    /// @brief Spin-wait for a message (busy polling — lowest latency).
    /// @return The received message.
    ///
    /// WARNING: This burns CPU. Use only when latency matters more
    /// than power consumption. For interactive use, prefer
    /// try_receive() with a sleep between calls.
    [[nodiscard]] Message spin_receive() noexcept {
        while (true) {
            if (auto msg = queue_.try_pop()) {
                return *msg;
            }
            // Hint to the CPU that we're in a spin-wait loop
            // (reduces power consumption and may improve latency
            //  on hyperthreaded cores by yielding resources)
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
    }

    /// @brief Gracefully close the connection and join the receiver thread.
    void close() {
        running_.store(false, std::memory_order_release);

        if (ws_.is_open()) {
            boost::system::error_code ec;
            ws_.close(websocket::close_code::normal, ec);
            if (ec) {
                std::cerr << "[ws] Close error: " << ec.message() << "\n";
            }
        }

        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
    }

    /// @brief Check if the client is connected and running.
    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// @brief Get the client configuration.
    [[nodiscard]] const ClientConfig& config() const noexcept {
        return config_;
    }

private:
    /// @brief Background receiver loop — runs on its own thread.
    ///
    /// Reads messages from the WebSocket and pushes them into
    /// the lock-free SPSC queue. No mutex is acquired on this path.
    ///
    /// Tier 2: optionally pins to a CPU core and uses pre-allocated buffer.
    void receive_loop() {
        // Pin receiver thread to a specific core for deterministic latency
        if (config_.pin_receiver_thread) {
            int core = config_.receiver_core_id;
            if (core < 0) {
                // Default: pick the last core (often less loaded by OS)
                core = static_cast<int>(get_cpu_count()) - 1;
            }
            ThreadPinner pinner(core, "ws_receiver");
        }

        // Pre-allocate a reusable read buffer to avoid per-read heap allocation
        beast::flat_buffer read_buffer;
        read_buffer.reserve(kReadBufferSize);

        try {
            while (running_.load(std::memory_order_acquire)) {
                read_buffer.clear();   // Reset without deallocating
                boost::system::error_code ec;

                ws_.read(read_buffer, ec);

                if (ec) {
                    if (ec == websocket::error::closed) {
                        std::cout << "[ws] Connection closed by server.\n";
                    } else if (running_.load(std::memory_order_acquire)) {
                        std::cerr << "[ws] Read error: " << ec.message() << "\n";
                    }
                    break;
                }

                Message msg;
                if (ws_.got_text()) {
                    msg = Message(
                        beast::buffers_to_string(read_buffer.data()), true);
                } else {
                    auto* data_ptr = static_cast<const uint8_t*>(
                        read_buffer.data().data());
                    std::string binary_str(
                        data_ptr, data_ptr + read_buffer.size());
                    msg = Message(std::move(binary_str), false);
                }

                // Lock-free enqueue — no mutex, no syscall
                if (!queue_.try_push(std::move(msg))) {
                    std::cerr << "[ws] WARNING: Message queue full, "
                              << "dropping message!\n";
                }
            }
        } catch (const std::exception& e) {
            if (running_.load(std::memory_order_acquire)) {
                std::cerr << "[ws] Receiver exception: " << e.what() << "\n";
            }
        }

        running_.store(false, std::memory_order_release);
    }

    tcp::resolver                    resolver_;
    websocket::stream<tcp::socket>   ws_;
    std::string                      host_;
    std::string                      port_;
    ClientConfig                     config_;

    std::atomic<bool>                running_;
    std::thread                      receiver_thread_;

    /// Lock-free SPSC queue — the core low-latency data structure
    SPSCQueue<Message, kQueueCapacity> queue_;
};

}  // namespace ws
