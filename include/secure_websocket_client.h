#pragma once

// Guard: This header requires OpenSSL. Compile with -DWS_ENABLE_SSL to enable.
#ifdef WS_ENABLE_SSL

// TLS/SSL WebSocket client (wss://).
// Same lock-free SPSC architecture as WebSocketClient, but wrapped in an SSL stream.
// TLS adds ~1-2ms to connection setup, zero overhead per-message after handshake.

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
#endif

#include "memory_pool.h"
#include "message.h"
#include "spsc_queue.h"
#include "thread_utils.h"

namespace ws {

namespace net       = boost::asio;
namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace ssl       = net::ssl;
using tcp           = net::ip::tcp;

class SecureWebSocketClient {
public:
    SecureWebSocketClient(net::io_context& ioc,
                          ssl::context& ssl_ctx,
                          const std::string& host,
                          const std::string& port,
                          ClientConfig config = {})
        : resolver_(ioc)
        , ws_(ioc, ssl_ctx)
        , host_(host)
        , port_(port)
        , config_(config)
        , running_(false) {}

    ~SecureWebSocketClient() {
        close();
    }

    SecureWebSocketClient(const SecureWebSocketClient&) = delete;
    SecureWebSocketClient& operator=(const SecureWebSocketClient&) = delete;
    SecureWebSocketClient(SecureWebSocketClient&&) = delete;
    SecureWebSocketClient& operator=(SecureWebSocketClient&&) = delete;

    // DNS resolve → TCP connect → TLS handshake → WebSocket upgrade
    void connect() {
        auto const results = resolver_.resolve(host_, port_);
        auto ep = net::connect(beast::get_lowest_layer(ws_), results);

        // SNI — required by most TLS servers for virtual hosting
        if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(),
                                       host_.c_str())) {
            boost::system::error_code ec{
                static_cast<int>(::ERR_get_error()),
                net::error::get_ssl_category()};
            throw boost::system::system_error{ec};
        }

        beast::get_lowest_layer(ws_).set_option(tcp::no_delay(true));

        beast::get_lowest_layer(ws_).set_option(
            boost::asio::socket_base::receive_buffer_size(
                static_cast<int>(kReadBufferSize)));
        beast::get_lowest_layer(ws_).set_option(
            boost::asio::socket_base::send_buffer_size(
                static_cast<int>(kReadBufferSize)));

        ws_.next_layer().handshake(ssl::stream_base::client);

        std::string ws_host = host_ + ":" + std::to_string(ep.port());
        ws_.handshake(ws_host, "/");

        running_.store(true, std::memory_order_release);

        std::cout << "[wss] Connected to " << host_ << ":" << port_
                  << " (TLS secured)\n";

        receiver_thread_ = std::thread(
            &SecureWebSocketClient::receive_loop, this);
    }

    void send_text(const std::string& message) {
        ws_.text(true);
        ws_.write(net::buffer(message));
    }

    void send_binary(const std::vector<uint8_t>& data) {
        ws_.binary(true);
        ws_.write(net::buffer(data));
    }

    [[nodiscard]] std::optional<Message> try_receive() noexcept {
        return queue_.try_pop();
    }

    [[nodiscard]] Message spin_receive() noexcept {
        while (true) {
            if (auto msg = queue_.try_pop()) {
                return *msg;
            }
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
    }

    void close() {
        running_.store(false, std::memory_order_release);

        if (ws_.is_open()) {
            boost::system::error_code ec;
            ws_.close(websocket::close_code::normal, ec);
            if (ec) {
                std::cerr << "[wss] Close error: " << ec.message() << "\n";
            }
        }

        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const ClientConfig& config() const noexcept {
        return config_;
    }

private:
    void receive_loop() {
        if (config_.pin_receiver_thread) {
            int core = config_.receiver_core_id;
            if (core < 0) {
                core = static_cast<int>(get_cpu_count()) - 1;
            }
            ThreadPinner pinner(core, "wss_receiver");
        }

        beast::flat_buffer read_buffer;
        read_buffer.reserve(kReadBufferSize);

        try {
            while (running_.load(std::memory_order_acquire)) {
                read_buffer.clear();
                boost::system::error_code ec;

                ws_.read(read_buffer, ec);

                if (ec) {
                    if (ec == websocket::error::closed) {
                        std::cout << "[wss] Connection closed by server.\n";
                    } else if (running_.load(std::memory_order_acquire)) {
                        std::cerr << "[wss] Read error: "
                                  << ec.message() << "\n";
                    }
                    break;
                }

                Message msg;
                if (ws_.got_text()) {
                    msg = Message(
                        beast::buffers_to_string(read_buffer.data()), true);
                } else {
                    msg = Message(
                        beast::buffers_to_string(read_buffer.data()), false);
                }

                if (!queue_.try_push(std::move(msg))) {
                    std::cerr << "[wss] WARNING: Message queue full, "
                              << "dropping message!\n";
                }
            }
        } catch (const std::exception& e) {
            if (running_.load(std::memory_order_acquire)) {
                std::cerr << "[wss] Receiver exception: "
                          << e.what() << "\n";
            }
        }

        running_.store(false, std::memory_order_release);
    }

    tcp::resolver                                              resolver_;
    websocket::stream<beast::ssl_stream<tcp::socket>>          ws_;
    std::string                                                host_;
    std::string                                                port_;
    ClientConfig                                               config_;

    std::atomic<bool>                                          running_;
    std::thread                                                receiver_thread_;

    SPSCQueue<Message, kQueueCapacity>                         queue_;
};

}  // namespace ws

#endif  // WS_ENABLE_SSL
