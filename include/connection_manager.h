#pragma once

// Resilient WebSocket connection with auto-reconnect and heartbeat.
// Wraps WebSocketClient with lifecycle management — separate from raw I/O
// to maintain single-responsibility.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <thread>

#include "websocket_client.h"

namespace ws {

struct ConnectionConfig {
    std::string host;
    std::string port;

    // Reconnection
    bool     auto_reconnect       = true;
    int      max_reconnect_tries  = 10;        // 0 = unlimited
    uint32_t initial_backoff_ms   = 100;
    uint32_t max_backoff_ms       = 30000;
    double   backoff_multiplier   = 2.0;

    // Heartbeat
    bool     heartbeat_enabled    = true;
    uint32_t heartbeat_interval_s = 30;
    uint32_t heartbeat_timeout_s  = 10;

    // Thread pinning
    bool     pin_receiver_thread  = false;
    int      receiver_core_id     = -1;        // -1 = auto-select last core
};

enum class ConnectionState : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Closing,
    Closed
};

inline const char* to_string(ConnectionState state) noexcept {
    switch (state) {
        case ConnectionState::Disconnected: return "DISCONNECTED";
        case ConnectionState::Connecting:   return "CONNECTING";
        case ConnectionState::Connected:    return "CONNECTED";
        case ConnectionState::Reconnecting: return "RECONNECTING";
        case ConnectionState::Closing:      return "CLOSING";
        case ConnectionState::Closed:       return "CLOSED";
        default:                            return "UNKNOWN";
    }
}

using OnConnectCallback    = std::function<void()>;
using OnDisconnectCallback = std::function<void(const std::string& reason)>;
using OnReconnectCallback  = std::function<void(int attempt, uint32_t backoff_ms)>;

class ConnectionManager {
public:
    explicit ConnectionManager(ConnectionConfig config)
        : config_(std::move(config))
        , state_(ConnectionState::Disconnected)
        , should_run_(false)
        , reconnect_count_(0)
        , ioc_()
        , client_(nullptr)
    {}

    ~ConnectionManager() {
        stop();
    }

    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    void start() {
        should_run_.store(true, std::memory_order_release);
        set_state(ConnectionState::Connecting);

        if (try_connect()) {
            set_state(ConnectionState::Connected);
            reconnect_count_ = 0;
        } else if (config_.auto_reconnect) {
            set_state(ConnectionState::Reconnecting);
            reconnect_thread_ = std::thread(&ConnectionManager::reconnect_loop, this);
            return;
        } else {
            set_state(ConnectionState::Disconnected);
            return;
        }

        if (config_.heartbeat_enabled) {
            heartbeat_thread_ = std::thread(&ConnectionManager::heartbeat_loop, this);
        }

        monitor_thread_ = std::thread(&ConnectionManager::monitor_loop, this);
    }

    void stop() {
        if (!should_run_.load(std::memory_order_acquire)) return;

        set_state(ConnectionState::Closing);
        should_run_.store(false, std::memory_order_release);

        if (client_) {
            client_->close();
        }

        if (heartbeat_thread_.joinable())  heartbeat_thread_.join();
        if (monitor_thread_.joinable())    monitor_thread_.join();
        if (reconnect_thread_.joinable())  reconnect_thread_.join();

        set_state(ConnectionState::Closed);
    }

    [[nodiscard]] WebSocketClient* client() noexcept { return client_.get(); }

    [[nodiscard]] ConnectionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return state_.load(std::memory_order_acquire) == ConnectionState::Connected;
    }

    [[nodiscard]] int reconnect_count() const noexcept { return reconnect_count_; }

    void on_connect(OnConnectCallback cb)       { on_connect_ = std::move(cb); }
    void on_disconnect(OnDisconnectCallback cb)  { on_disconnect_ = std::move(cb); }
    void on_reconnect(OnReconnectCallback cb)    { on_reconnect_ = std::move(cb); }

private:
    bool try_connect() {
        try {
            client_ = std::make_unique<WebSocketClient>(ioc_, config_.host, config_.port);
            client_->connect();

            if (on_connect_) on_connect_();
            return true;

        } catch (const std::exception& e) {
            std::cerr << "[conn] Connection failed: " << e.what() << "\n";
            client_.reset();
            return false;
        }
    }

    // Backoff: 100ms → 200ms → 400ms → ... → max_backoff_ms
    void reconnect_loop() {
        uint32_t backoff_ms = config_.initial_backoff_ms;

        while (should_run_.load(std::memory_order_acquire)) {
            ++reconnect_count_;

            if (config_.max_reconnect_tries > 0 &&
                reconnect_count_ > config_.max_reconnect_tries) {
                std::cerr << "[conn] Max reconnection attempts ("
                          << config_.max_reconnect_tries << ") exceeded.\n";
                set_state(ConnectionState::Disconnected);

                if (on_disconnect_) on_disconnect_("max retries exceeded");
                return;
            }

            std::cout << "[conn] Reconnecting (attempt " << reconnect_count_
                      << ", backoff " << backoff_ms << "ms)...\n";

            if (on_reconnect_) on_reconnect_(reconnect_count_, backoff_ms);

            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));

            if (try_connect()) {
                std::cout << "[conn] Reconnected after " << reconnect_count_
                          << " attempt(s).\n";
                set_state(ConnectionState::Connected);
                reconnect_count_ = 0;

                if (config_.heartbeat_enabled && !heartbeat_thread_.joinable()) {
                    heartbeat_thread_ = std::thread(&ConnectionManager::heartbeat_loop, this);
                }

                if (!monitor_thread_.joinable()) {
                    monitor_thread_ = std::thread(&ConnectionManager::monitor_loop, this);
                }
                return;
            }

            // Exponential backoff with cap
            backoff_ms = std::min(
                static_cast<uint32_t>(backoff_ms * config_.backoff_multiplier),
                config_.max_backoff_ms
            );
        }
    }

    // Polls for connection drops, triggers reconnect
    void monitor_loop() {
        while (should_run_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            if (!should_run_.load(std::memory_order_acquire)) break;

            if (client_ && !client_->is_running()) {
                std::cout << "[conn] Connection lost, initiating reconnect...\n";
                set_state(ConnectionState::Reconnecting);

                if (on_disconnect_) on_disconnect_("connection lost");

                client_.reset();

                if (config_.auto_reconnect) {
                    reconnect_loop();
                } else {
                    set_state(ConnectionState::Disconnected);
                }
                return;
            }
        }
    }

    // Sends periodic pings; if connection drops, receiver thread's
    // read error triggers reconnection via the monitor
    void heartbeat_loop() {
        while (should_run_.load(std::memory_order_acquire) &&
               state_.load(std::memory_order_acquire) == ConnectionState::Connected) {

            std::this_thread::sleep_for(
                std::chrono::seconds(config_.heartbeat_interval_s));

            if (!should_run_.load(std::memory_order_acquire)) break;
            if (!client_ || !client_->is_running()) break;

            try {
                client_->send_text("__ping__");
            } catch (const std::exception& e) {
                std::cerr << "[heartbeat] Ping failed: " << e.what() << "\n";
                break;
            }
        }
    }

    void set_state(ConnectionState new_state) {
        auto old_state = state_.exchange(new_state, std::memory_order_acq_rel);
        if (old_state != new_state) {
            std::cout << "[conn] " << to_string(old_state)
                      << " → " << to_string(new_state) << "\n";
        }
    }

    ConnectionConfig       config_;
    std::atomic<ConnectionState> state_;
    std::atomic<bool>      should_run_;
    int                    reconnect_count_;

    net::io_context        ioc_;
    std::unique_ptr<WebSocketClient> client_;

    std::thread            heartbeat_thread_;
    std::thread            monitor_thread_;
    std::thread            reconnect_thread_;

    OnConnectCallback      on_connect_;
    OnDisconnectCallback   on_disconnect_;
    OnReconnectCallback    on_reconnect_;
};

}  // namespace ws
