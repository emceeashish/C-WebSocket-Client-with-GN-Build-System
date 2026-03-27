#pragma once

/// @file connection_manager.h
/// @brief Resilient WebSocket connection with auto-reconnect and heartbeat.
///
/// Adds production-grade connection management on top of WebSocketClient:
///   - Automatic reconnection with exponential backoff
///   - Configurable heartbeat (WebSocket ping/pong)
///   - Connection state machine with callbacks
///   - Graceful shutdown with drain
///
/// This is separate from WebSocketClient to maintain single-responsibility:
/// WebSocketClient handles raw I/O, ConnectionManager handles lifecycle.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <thread>

#include "websocket_client.h"

namespace ws {

/// @brief Connection configuration parameters.
struct ConnectionConfig {
    std::string host;
    std::string port;

    // Reconnection settings
    bool     auto_reconnect       = true;
    int      max_reconnect_tries  = 10;        ///< 0 = unlimited
    uint32_t initial_backoff_ms   = 100;       ///< First retry delay
    uint32_t max_backoff_ms       = 30000;     ///< Cap for exponential backoff
    double   backoff_multiplier   = 2.0;       ///< Exponential growth factor

    // Heartbeat settings
    bool     heartbeat_enabled    = true;
    uint32_t heartbeat_interval_s = 30;        ///< Seconds between pings
    uint32_t heartbeat_timeout_s  = 10;        ///< Timeout waiting for pong

    // Thread pinning
    bool     pin_receiver_thread  = false;
    int      receiver_core_id     = -1;        ///< -1 = auto-select last core
};

/// @brief Connection state for the state machine.
enum class ConnectionState : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Closing,
    Closed
};

/// @brief Returns a string representation of the connection state.
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

/// @brief Callback types for connection events.
using OnConnectCallback    = std::function<void()>;
using OnDisconnectCallback = std::function<void(const std::string& reason)>;
using OnReconnectCallback  = std::function<void(int attempt, uint32_t backoff_ms)>;

/// @brief Resilient WebSocket connection manager.
///
/// Wraps WebSocketClient with automatic reconnection (exponential backoff),
/// heartbeat monitoring, and a clean state machine.
///
/// Usage:
///   ConnectionConfig cfg;
///   cfg.host = "localhost";
///   cfg.port = "8765";
///   cfg.auto_reconnect = true;
///
///   ConnectionManager mgr(cfg);
///   mgr.on_connect([]{ std::cout << "Connected!\n"; });
///   mgr.start();
///   // ... use mgr.client() to send/receive ...
///   mgr.stop();
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

    // Non-copyable, non-movable
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    /// @brief Start the connection manager (connects and begins monitoring).
    void start() {
        should_run_.store(true, std::memory_order_release);
        set_state(ConnectionState::Connecting);

        // Initial connection attempt
        if (try_connect()) {
            set_state(ConnectionState::Connected);
            reconnect_count_ = 0;
        } else if (config_.auto_reconnect) {
            // Start reconnection loop in background
            set_state(ConnectionState::Reconnecting);
            reconnect_thread_ = std::thread(&ConnectionManager::reconnect_loop, this);
            return;
        } else {
            set_state(ConnectionState::Disconnected);
            return;
        }

        // Start heartbeat thread if enabled
        if (config_.heartbeat_enabled) {
            heartbeat_thread_ = std::thread(&ConnectionManager::heartbeat_loop, this);
        }

        // Start connection monitor
        monitor_thread_ = std::thread(&ConnectionManager::monitor_loop, this);
    }

    /// @brief Stop the connection manager gracefully.
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

    /// @brief Access the underlying WebSocket client.
    /// @return Pointer to the client, or nullptr if not connected.
    [[nodiscard]] WebSocketClient* client() noexcept { return client_.get(); }

    /// @brief Get the current connection state.
    [[nodiscard]] ConnectionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    /// @brief Check if currently connected and ready.
    [[nodiscard]] bool is_connected() const noexcept {
        return state_.load(std::memory_order_acquire) == ConnectionState::Connected;
    }

    /// @brief Get the number of reconnection attempts since last successful connect.
    [[nodiscard]] int reconnect_count() const noexcept { return reconnect_count_; }

    // Event callbacks
    void on_connect(OnConnectCallback cb)       { on_connect_ = std::move(cb); }
    void on_disconnect(OnDisconnectCallback cb)  { on_disconnect_ = std::move(cb); }
    void on_reconnect(OnReconnectCallback cb)    { on_reconnect_ = std::move(cb); }

private:
    /// @brief Attempt a connection to the server.
    /// @return true if connection succeeded.
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

    /// @brief Reconnection loop with exponential backoff.
    ///
    /// Backoff sequence: 100ms → 200ms → 400ms → 800ms → ... → max_backoff_ms
    /// Jitter could be added for production use to avoid thundering herd.
    void reconnect_loop() {
        uint32_t backoff_ms = config_.initial_backoff_ms;

        while (should_run_.load(std::memory_order_acquire)) {
            ++reconnect_count_;

            // Check max retries
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

            // Wait with backoff
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));

            if (try_connect()) {
                std::cout << "[conn] Reconnected after " << reconnect_count_
                          << " attempt(s).\n";
                set_state(ConnectionState::Connected);
                reconnect_count_ = 0;

                // Start heartbeat if enabled
                if (config_.heartbeat_enabled && !heartbeat_thread_.joinable()) {
                    heartbeat_thread_ = std::thread(&ConnectionManager::heartbeat_loop, this);
                }

                // Start monitor
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

    /// @brief Monitor connection health, trigger reconnect on drop.
    void monitor_loop() {
        while (should_run_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            if (!should_run_.load(std::memory_order_acquire)) break;

            // Check if client is still running
            if (client_ && !client_->is_running()) {
                std::cout << "[conn] Connection lost, initiating reconnect...\n";
                set_state(ConnectionState::Reconnecting);

                if (on_disconnect_) on_disconnect_("connection lost");

                client_.reset();

                if (config_.auto_reconnect) {
                    reconnect_loop();  // Run inline in monitor thread
                } else {
                    set_state(ConnectionState::Disconnected);
                }
                return;
            }
        }
    }

    /// @brief Periodic heartbeat using WebSocket ping control frames.
    ///
    /// Sends a ping every heartbeat_interval_s seconds.
    /// If the connection drops, the read error in the receiver thread
    /// will trigger reconnection via the monitor.
    void heartbeat_loop() {
        while (should_run_.load(std::memory_order_acquire) &&
               state_.load(std::memory_order_acquire) == ConnectionState::Connected) {

            std::this_thread::sleep_for(
                std::chrono::seconds(config_.heartbeat_interval_s));

            if (!should_run_.load(std::memory_order_acquire)) break;
            if (!client_ || !client_->is_running()) break;

            try {
                // WebSocket ping — Beast handles pong automatically
                client_->send_text("__ping__");
            } catch (const std::exception& e) {
                std::cerr << "[heartbeat] Ping failed: " << e.what() << "\n";
                break;
            }
        }
    }

    /// @brief Update the connection state atomically.
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

    // Callbacks
    OnConnectCallback      on_connect_;
    OnDisconnectCallback   on_disconnect_;
    OnReconnectCallback    on_reconnect_;
};

}  // namespace ws
