# Low Latency WebSocket Client

**C++17 · Boost.Beast · Lock-Free · GN+Ninja**

A high performance WebSocket client engineered for low latency communication. Features a lock-free SPSC ring buffer, CPU thread pinning, memory pooling, auto-reconnect with exponential backoff, and a comprehensive benchmarking suite.

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                    Main Thread (Consumer)                        │
│                                                                  │
│  send_text() ──────────► WebSocket ──────────► Server            │
│                           (TCP_NODELAY)                          │
│  spin_receive() ◄── SPSC Queue ◄── Receiver Thread (Producer)    │
│     (lock-free)      (no mutex)     (pinned to CPU core N)       │
│                                                                  │
│  LatencyStats   → per-msg RTT, p50/p95/p99/p99.9                │
│  MemoryPool     → O(1) alloc/dealloc, zero heap on hot path     │
│  ConnManager    → auto-reconnect, exp. backoff, heartbeat        │
│  Config         → constexpr policies, compile-time feature flags │
└──────────────────────────────────────────────────────────────────┘
```

## Project Structure

```
├── include/
│   ├── websocket_client.h      # Client — Beast + SPSC + thread pinning
│   ├── secure_websocket_client.h # TLS/SSL client (wss://)
│   ├── spsc_queue.h            # Lock-free SPSC ring buffer
│   ├── latency_stats.h         # RTT + percentile statistics
│   ├── memory_pool.h           # Fixed-block O(1) free-list allocator
│   ├── thread_utils.h          # CPU affinity + thread naming
│   ├── connection_manager.h    # Auto-reconnect + heartbeat
│   ├── config.h                # Compile-time config + template policies
│   └── message.h               # Message type definition
├── src/
│   └── main.cpp                # Entry point, CLI, chat session
├── benchmarks/
│   └── benchmark_client.cpp    # 4-scenario latency benchmark suite
├── tests/
│   ├── test_spsc_queue.cpp     # 10 tests (incl. 100K concurrent)
│   ├── test_memory_pool.cpp    # 12 tests (incl. RAII, move, cycles)
│   └── test_latency_stats.cpp  # 10 tests
├── .github/workflows/ci.yml   # GitHub Actions (Linux + Windows)
├── BUILD.gn                    # GN build (client + bench + tests)
├── websocket_server.py         # Python echo server for testing
└── .gitignore
```

## Key Design Decisions

| Decision | Rationale |
|---|---|
| **Lock-free SPSC queue** | Zero mutex contention; single producer / single consumer |
| **Cache-line alignment** (`alignas(64)`) | Prevents false sharing between head/tail |
| **Power-of-two capacity** | Branchless modulo via bitwise AND |
| **`TCP_NODELAY`** | Disables Nagle's algorithm for immediate sends |
| **`_mm_pause()` spin hint** | Efficient x86 busy-wait, yields to HT sibling |
| **CPU affinity** | Eliminates core migration, keeps L1/L2 warm |
| **Free-list memory pool** | O(1) alloc/dealloc, no heap fragmentation |
| **Acquire-release ordering** | Minimal fence cost vs. sequential consistency |
| **Exponential backoff** | Production reconnect: 100ms → 200ms → ... → 30s |
| **Template wait policies** | Compile-time swap: spin / yield / sleep (zero cost) |
| **`constexpr` configuration** | All tunables resolved at compile time |
| **TLS/SSL support** | Optional `wss://` via Boost.Beast SSL, TLSv1.2+, SNI, system CA |

## Build

```bash
# GN + Ninja
gn gen out/Default
ninja -C out/Default

# Or direct g++ compilation
g++ -std=c++17 -O2 -Wall -Wextra -I include \
    src/main.cpp -lboost_system -lboost_thread -lpthread \
    -o websocket_client

# Build with TLS/SSL support (requires OpenSSL)
g++ -std=c++17 -O2 -Wall -Wextra -DWS_ENABLE_SSL -I include \
    src/main.cpp -lboost_system -lboost_thread -lssl -lcrypto -lpthread \
    -o websocket_client_ssl
```

## Usage

```bash
# Default echo server
./websocket_client

# Custom server with thread pinning + auto-reconnect
./websocket_client --host localhost --port 8765 --pin 3 --reconnect

# Secure WebSocket (TLS) — requires SSL build
./websocket_client_ssl --ssl --host echo.websocket.events

# Run benchmark suite
./benchmark_client
./benchmark_client localhost 8765

# Run unit tests
./test_spsc_queue && ./test_memory_pool && ./test_latency_stats
```

### CLI Options

| Flag | Description |
|---|---|
| `--host HOST` | Server hostname (default: echo.websocket.events) |
| `--port PORT` | Server port (default: 80, 443 with --ssl) |
| `--ssl` | Use TLS/SSL secure connection (wss://) |
| `--pin [CORE]` | Pin receiver thread to CPU core |
| `--reconnect` | Enable auto-reconnect on disconnect |
| `-h, --help` | Show help |

### Interactive Commands

| Command | Description |
|---|---|
| `text` | Send text message, show RTT |
| `binary` | Send binary message, show RTT |
| `bench` | Run 100-message latency benchmark |
| `stats` | Print cumulative latency distribution |
| `pool` | Print memory pool status |
| `sysinfo` | Print system configuration |
| `/exit` | Quit and print final report |

## Benchmark Suite

The dedicated benchmark (`benchmarks/benchmark_client.cpp`) runs 4 scenarios:

| Scenario | What It Measures |
|---|---|
| **Sequential RTT** | Per-message round-trip latency (send → receive) |
| **Throughput** | Burst mode: messages/second, total time |
| **Warm-Up Effect** | Cold start vs. warm state latency comparison |
| **Message Size Impact** | RTT across payloads: 16B → 16KB |

### Sample Output

```
┌─────────┬──────────┬──────────┬──────────┬──────────┐
│  Size   │  Min µs  │  Mean µs │  p99 µs  │  Max µs  │
├─────────┼──────────┼──────────┼──────────┼──────────┤
│    16 B │    125.3 │    198.4 │    892.1 │   1203.5 │
│    64 B │    131.7 │    205.6 │    934.8 │   1342.9 │
│   256 B │    142.1 │    231.2 │   1023.4 │   1567.8 │
│  1024 B │    168.5 │    287.9 │   1198.2 │   1845.3 │
│  4096 B │    234.2 │    412.7 │   1567.3 │   2341.6 │
│ 16384 B │    412.8 │    678.4 │   2134.5 │   3456.2 │
└─────────┴──────────┴──────────┴──────────┴──────────┘
```

## Testing

32 unit tests across 3 test suites — all implemented with zero external dependencies.

| Suite | Tests | Key Coverage |
|---|---|---|
| `test_spsc_queue` | 10 | FIFO order, wrap-around, full/empty, move semantics, **100K concurrent correctness** |
| `test_memory_pool` | 12 | Alloc/dealloc, exhaustion, RAII PoolPtr, move, ownership, multi-cycle |
| `test_latency_stats` | 10 | Recording, percentiles, reset, pre-allocation, clock monotonicity |

## CI/CD

GitHub Actions runs on every push and PR:
- **Linux** (ubuntu-latest): full build + all tests
- **Windows** (windows-latest): unit tests
