// Entry point for the low latency WebSocket client.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(WS_ENABLE_SSL) && defined(_WIN32)
    #include <openssl/ssl.h>
    #include <openssl/x509.h>
    #include <wincrypt.h>
    // Conflicts with OpenSSL/Boost type names
    #ifdef X509_NAME
        #undef X509_NAME
    #endif
#endif

#include "connection_manager.h"
#include "latency_stats.h"
#include "memory_pool.h"
#include "secure_websocket_client.h"
#include "thread_utils.h"
#include "websocket_client.h"

void print_system_info() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════╗\n";
    std::cout << "║   Low Latency WebSocket Client v2.0         ║\n";
    std::cout << "╠══════════════════════════════════════════════╣\n";
    std::cout << "║  CPU cores     : " << ws::get_cpu_count() << std::string(28 - std::to_string(ws::get_cpu_count()).size(), ' ') << "║\n";
    std::cout << "║  Queue capacity: " << ws::kQueueCapacity << std::string(28 - std::to_string(ws::kQueueCapacity).size(), ' ') << "║\n";
    std::cout << "║  Read buffer   : " << ws::kReadBufferSize / 1024 << " KB" << std::string(25 - std::to_string(ws::kReadBufferSize / 1024).size(), ' ') << "║\n";
    std::cout << "║  Pool blocks   : " << ws::kPoolNumBlocks << " x " << ws::kPoolBlockSize << " B"
              << std::string(22 - std::to_string(ws::kPoolNumBlocks).size() - std::to_string(ws::kPoolBlockSize).size(), ' ') << "║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n\n";
}

// Templated so it works with both WebSocketClient and SecureWebSocketClient
template <typename ClientType>
void chat_session(ClientType& client) {
    ws::LatencyStats stats;

    ws::MemoryPool<ws::kPoolBlockSize, ws::kPoolNumBlocks> msg_pool;
    std::cout << "[pool] Memory pool initialized: "
              << msg_pool.capacity() << " blocks x "
              << msg_pool.block_size() << " bytes = "
              << (msg_pool.capacity() * msg_pool.block_size()) / 1024
              << " KB total\n";

    std::cout << "[*] Waiting for server greeting...\n";
    for (int i = 0; i < 100; ++i) {
        if (auto msg = client.try_receive()) {
            std::cout << "[server] " << msg->payload << "\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "\n";
    std::cout << "Commands:\n";
    std::cout << "  text    — Send a text message\n";
    std::cout << "  binary  — Send a binary message\n";
    std::cout << "  bench   — Run latency benchmark (100 messages)\n";
    std::cout << "  stats   — Print latency statistics\n";
    std::cout << "  pool    — Print memory pool status\n";
    std::cout << "  sysinfo — Print system configuration\n";
    std::cout << "  /exit   — Quit\n\n";

    std::string input;
    while (client.is_running()) {
        std::cout << "> ";
        if (!std::getline(std::cin, input)) break;

        if (input == "/exit") break;
        if (input.empty()) continue;

        if (input == "stats") {
            stats.print_report("Round-Trip Latency");
            continue;
        }

        if (input == "pool") {
            std::cout << "[pool] Free: " << msg_pool.free_count()
                      << " / " << msg_pool.capacity()
                      << " blocks (" << msg_pool.free_count() * msg_pool.block_size() / 1024
                      << " KB available)\n";
            continue;
        }

        if (input == "sysinfo") {
            print_system_info();
            continue;
        }

        if (input == "bench") {
            constexpr int kBenchMessages = 100;
            std::cout << "[bench] Sending " << kBenchMessages
                      << " messages for latency measurement...\n";

            ws::LatencyStats bench_stats(kBenchMessages);

            for (int i = 0; i < kBenchMessages; ++i) {
                ws::PoolPtr<decltype(msg_pool)> buf(msg_pool);
                if (!buf) {
                    std::cerr << "[bench] Pool exhausted at message " << i << "\n";
                    break;
                }

                std::string msg = "bench_" + std::to_string(i);

                std::memcpy(buf.get(), msg.data(),
                            std::min(msg.size(), ws::kPoolBlockSize));

                auto t_start = ws::LatencyStats::now();
                client.send_text(msg);

                auto response = client.spin_receive();
                bench_stats.record(t_start);
            }

            bench_stats.print_report("Benchmark Latency (" +
                                     std::to_string(kBenchMessages) + " msgs)");
            continue;
        }

        if (input == "text") {
            std::cout << "Message: ";
            if (!std::getline(std::cin, input)) break;

            auto t_start = ws::LatencyStats::now();
            client.send_text(input);

            auto response = client.spin_receive();
            stats.record(t_start);

            auto rtt_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              ws::LatencyStats::now() - t_start).count();

            std::cout << "[server:text] " << response.payload
                      << "  (RTT: " << rtt_us << " µs)\n";

        } else if (input == "binary") {
            std::cout << "Data: ";
            if (!std::getline(std::cin, input)) break;

            std::vector<uint8_t> binary_data(input.begin(), input.end());

            auto t_start = ws::LatencyStats::now();
            client.send_binary(binary_data);

            auto response = client.spin_receive();
            stats.record(t_start);

            auto rtt_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              ws::LatencyStats::now() - t_start).count();

            std::cout << "[server:binary] " << response.payload
                      << "  (RTT: " << rtt_us << " µs)\n";

        } else {
            std::cout << "Unknown command. Type 'text', 'binary', 'bench', "
                      << "'stats', 'pool', 'sysinfo', or '/exit'.\n";
        }
    }

    if (stats.count() > 0) {
        stats.print_report("Session Latency Summary");
    }

    client.close();
}

struct CliArgs {
    std::string host         = "echo.websocket.events";
    std::string port         = "80";
    bool        pin_thread   = false;
    int         pin_core     = -1;
    bool        auto_reconnect = false;
    bool        use_ssl      = false;
};

CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--host" && i + 1 < argc) {
            args.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            args.port = argv[++i];
        } else if (arg == "--pin") {
            args.pin_thread = true;
            if (i + 1 < argc) {
                try {
                    args.pin_core = std::stoi(argv[i + 1]);
                    ++i;
                } catch (...) {}
            }
        } else if (arg == "--reconnect") {
            args.auto_reconnect = true;
        } else if (arg == "--ssl") {
            args.use_ssl = true;
            if (args.port == "80") args.port = "443";
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: websocket_client [OPTIONS]\n\n"
                      << "Options:\n"
                      << "  --host HOST      Server hostname (default: echo.websocket.events)\n"
                      << "  --port PORT      Server port (default: 80, 443 with --ssl)\n"
                      << "  --ssl            Use TLS/SSL (wss://) connection\n"
                      << "  --pin [CORE]     Pin receiver thread to CPU core\n"
                      << "  --reconnect      Enable auto-reconnect on disconnect\n"
                      << "  -h, --help       Show this help\n\n"
                      << "Examples:\n"
                      << "  websocket_client --host localhost --port 8765\n"
                      << "  websocket_client --ssl --host echo.websocket.events\n"
                      << "  websocket_client --pin 3 --reconnect\n";
            std::exit(0);
        } else if (args.host == "echo.websocket.events" && i == 1) {
            args.host = arg;
        } else if (args.port == "80" && i == 2) {
            args.port = arg;
        }
    }

    return args;
}

int main(int argc, char* argv[]) {
    auto args = parse_args(argc, argv);

    print_system_info();

    ws::ClientConfig client_config;
    client_config.pin_receiver_thread = args.pin_thread;
    client_config.receiver_core_id    = args.pin_core;

    if (args.auto_reconnect) {
        ws::ConnectionConfig conn_config;
        conn_config.host = args.host;
        conn_config.port = args.port;
        conn_config.auto_reconnect = true;
        conn_config.initial_backoff_ms = 100;
        conn_config.max_backoff_ms = 30000;
        conn_config.heartbeat_enabled = true;
        conn_config.heartbeat_interval_s = 30;
        conn_config.pin_receiver_thread = args.pin_thread;
        conn_config.receiver_core_id = args.pin_core;

        ws::ConnectionManager mgr(conn_config);

        mgr.on_connect([] {
            std::cout << "[event] Connected successfully.\n";
        });
        mgr.on_disconnect([](const std::string& reason) {
            std::cout << "[event] Disconnected: " << reason << "\n";
        });
        mgr.on_reconnect([](int attempt, uint32_t backoff_ms) {
            std::cout << "[event] Reconnecting (attempt " << attempt
                      << ", next in " << backoff_ms << "ms)...\n";
        });

        std::cout << "[*] Connecting to " << args.host << ":" << args.port
                  << " (auto-reconnect enabled)...\n";
        mgr.start();

        for (int i = 0; i < 100 && !mgr.is_connected(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (mgr.is_connected() && mgr.client()) {
            chat_session(*mgr.client());
        } else {
            std::cerr << "[error] Could not establish connection.\n";
        }

        mgr.stop();

#ifdef WS_ENABLE_SSL
    } else if (args.use_ssl) {
        try {
            boost::asio::io_context ioc;

            boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);

#ifdef _WIN32
            // Windows: set_default_verify_paths() doesn't work,
            // so we load certs from the Windows Certificate Store
            ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
            HCERTSTORE hStore = CertOpenSystemStoreA(0, "ROOT");
            if (hStore) {
                X509_STORE* store = SSL_CTX_get_cert_store(ssl_ctx.native_handle());
                PCCERT_CONTEXT pContext = nullptr;
                while ((pContext = CertEnumCertificatesInStore(hStore, pContext))) {
                    X509* x509 = d2i_X509(nullptr,
                        const_cast<const unsigned char**>(&pContext->pbCertEncoded),
                        static_cast<long>(pContext->cbCertEncoded));
                    if (x509) {
                        X509_STORE_add_cert(store, x509);
                        X509_free(x509);
                    }
                }
                CertCloseStore(hStore, 0);
                std::cout << "[tls] Loaded certificates from Windows Certificate Store\n";
            }
#else
            ssl_ctx.set_default_verify_paths();
            ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
#endif

            ws::SecureWebSocketClient client(ioc, ssl_ctx, args.host, args.port, client_config);

            std::cout << "[*] Connecting to " << args.host << ":" << args.port << " (TLS)...\n";
            client.connect();

            chat_session(client);

        } catch (const boost::system::system_error& e) {
            std::cerr << "[error] TLS/Network error: " << e.what() << "\n";
            return 1;
        } catch (const std::exception& e) {
            std::cerr << "[error] " << e.what() << "\n";
            return 1;
        }
#else
    } else if (args.use_ssl) {
        std::cerr << "[error] SSL support not compiled. "
                  << "Rebuild with -DWS_ENABLE_SSL and link OpenSSL.\n";
        return 1;
#endif

    } else {
        try {
            boost::asio::io_context ioc;
            ws::WebSocketClient client(ioc, args.host, args.port, client_config);

            std::cout << "[*] Connecting to " << args.host << ":" << args.port << "...\n";
            if (args.pin_thread) {
                std::cout << "[*] Receiver thread pinning: core "
                          << (args.pin_core < 0 ? static_cast<int>(ws::get_cpu_count()) - 1 : args.pin_core)
                          << "\n";
            }
            client.connect();

            chat_session(client);

        } catch (const boost::system::system_error& e) {
            std::cerr << "[error] Network error: " << e.what() << "\n";
            return 1;
        } catch (const std::exception& e) {
            std::cerr << "[error] " << e.what() << "\n";
            return 1;
        }
    }

    return 0;
}
