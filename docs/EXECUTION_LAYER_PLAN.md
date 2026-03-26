# Plan: Execution Layer (Track E)

## Context

Brain detects arb crosses and currently only logs them to stderr + JSONL. This plan adds
a full execution path: brain broadcasts signals over a new outbound WS channel, and a new
`exec` process (one per venue, co-located with PoP on the same host) receives signals,
applies all E1–E5 safety guards, and submits orders via a pluggable `IOrderClient`
interface (stub for now; real exchange REST adapters plugged in later).

**Deployment topology:**
```
Binance host:  pop(binance)  exec(binance)  ──┐
OKX host:      pop(okx)      exec(okx)      ──┤──► Brain (central) ──► SignalServer
Bybit host:    pop(bybit)    exec(bybit)    ──┘         (port 8444)
...
```

---

## New Components

### 1. `SignalServer` (brain-side outbound WS push)

**Files:**
- `brain/include/brain/SignalServer.hpp` (new)
- `brain/src/brain/SignalServer.cpp` (new)

Outbound-only Boost.Beast WS server on `--signal-port` (default 8444).
- `broadcast(std::string text)` — sends a text frame to all active sessions
- Each `SignalSession` has an outbox queue + strand (same pattern as `WsClient`)
- Accept loop with mTLS optional (shares brain's existing CA cert)
- Sessions are read-and-discard inbound (exec sends no data up)
- Max 10 exec subscribers (same constant as WsServer)

### 2. `exec` subproject (new, one process per venue)

```
exec/
├── CMakeLists.txt
├── include/exec/
│   ├── ExecCmdLine.hpp        — CLI: venue, brain-signal host/port, guard + strategy flags
│   ├── IOrderClient.hpp       — pure interface: submit_order(), cancel_order()
│   ├── StubOrderClient.hpp    — logs intent, immediate synthetic fill (no real orders)
│   ├── IExecStrategy.hpp      — pure interface: on_signal(ArbCross), pause(), resume()
│   ├── ImmediateStrategy.hpp  — single market order, level-0 qty
│   ├── ThresholdStrategy.hpp  — spread-filter wrapper around any inner IExecStrategy
│   ├── SliceStrategy.hpp      — sliced exec: equal / decay / weighted sizing modes
│   ├── ExecEngine.hpp         — orchestrator: E1–E4 guards + strategy_->on_signal()
│   └── OrderTracker.hpp       — E5: pending order table + Asio confirmation timers
├── src/exec/
│   ├── Strategies.cpp         — implementations of all three strategy classes
│   ├── ExecEngine.cpp
│   └── OrderTracker.cpp
└── app/exec.cpp               — main(): strategy factory + ExecEngine + WsClient
```

---

## Detailed Design

### Signal flow (brain side)

`ArbDetector::emit_()` gains a callback hook:
```cpp
// ArbDetector.hpp — add:
std::function<void(const ArbCross &)> on_cross_;

// ArbDetector.cpp emit_() — add after file write:
if (on_cross_) on_cross_(c);
```

`brain.cpp` wires up the callback:
```cpp
arb.on_cross_ = [&signal_server](const ArbCross &c) {
    signal_server.broadcast(serialize_cross(c));   // same JSON as JSONL output
};
```

`BrainCmdLine.hpp` gains:
```
--signal-port N        (default 0 = disabled)
--signal-certfile      (mTLS: cert for signal server)
--signal-keyfile
--signal-ca-certfile   (mTLS: require exec clients to present signed cert)
```

### Signal flow (exec side)

`exec` connects to brain's signal port using the existing `WsClient`:
```
WsClient → brain SignalServer → receives ArbCross JSON frames
```

`ExecEngine::on_signal(json)`:
1. Parse ArbCross from JSON
2. Check `sell_venue == my_venue_ || buy_venue == my_venue_` — ignore others
3. Determine leg: SELL (if sell_venue == my_venue_) or BUY (if buy_venue == my_venue_)
4. Compute order notional = price_tick/100 × quantity
5. **E4** fat-finger: reject if notional > `max_order_notional`
6. **E1** position limit: reject if open_notional + new_notional > `position_limit`
7. **E2** kill switch: reject if `paused_` is true
8. **E3** dedup: reject if same (sell_venue, buy_venue) pair seen within cooldown window
9. Submit via `IOrderClient::submit_order(venue, side, price_tick, quantity)`
10. **E5** register in `OrderTracker` with confirmation deadline

### `IOrderClient` interface
```cpp
struct Order {
    std::string venue;
    Side side;               // BUY / SELL
    std::int64_t price_tick;
    double quantity;
    std::string client_order_id;
};

struct Fill {
    std::string client_order_id;
    double filled_qty;
    std::int64_t fill_price_tick;
    std::int64_t ts_ns;
};

class IOrderClient {
public:
    virtual ~IOrderClient() = default;
    virtual void submit_order(const Order &o, std::function<void(std::error_code, Fill)> cb) = 0;
    virtual void cancel_order(const std::string &client_order_id,
                              std::function<void(std::error_code)> cb) = 0;
};
```

### `StubOrderClient`
Logs the order to spdlog at `info` level, then fires the callback immediately with a
synthetic `Fill` at the requested price and full quantity. No network I/O.

### `IExecStrategy` — pluggable execution strategy

Generic behavioral interface. `ExecEngine` knows nothing about *how* a strategy executes —
only that it receives a signal and is responsible for all order submission. The strategy
owns its own timing, sizing logic, and interaction with `IOrderClient`.

```cpp
class IExecStrategy {
public:
    virtual ~IExecStrategy() = default;

    // Called once per ArbCross after E1–E4 guards pass.
    // The strategy is fully responsible for submitting orders (via the IOrderClient
    // it received at construction) and scheduling any deferred work via the strand.
    // Must return immediately — all async work uses the Asio strand.
    virtual void on_signal(const ArbCross &cross) = 0;

    // E2 kill switch: strategy must stop submitting new orders when paused.
    virtual void pause() {}
    virtual void resume() {}
};
```

Each concrete strategy is constructed with:
- `IOrderClient &client` — order submission
- `OrderTracker &tracker` — E5 registration of pending orders
- `boost::asio::strand<...> &strand` — for scheduling deferred work (slices, timers)
- `StrategyConfig config` — parsed from CLI (target_qty, spread threshold, etc.)

`ExecEngine` owns `std::unique_ptr<IExecStrategy> strategy_`. Flow after guards:
```cpp
// ExecEngine::on_signal(), after E1–E4 pass:
strategy_->on_signal(cross);   // strategy handles everything from here
```

**Built-in implementations:**

| Strategy | `--strategy` | Description |
|---|---|---|
| `ImmediateStrategy` | `immediate` | Single market order per leg at level-0 qty (capped by `--target-qty`) |
| `ThresholdStrategy` | `threshold` | Wraps any inner strategy; skips if `spread_bps < --min-spread-bps` |
| `SliceStrategy` | `slice` | Submits N order slices per leg with configurable sizing and inter-slice delay |

`SliceStrategy` supports three sizing modes via `--slice-mode`:

| `--slice-mode` | Description |
|---|---|
| `equal` | N equal slices, each `total_qty / N` (set N with `--split-count`) |
| `decay` | Geometrically shrinking: each slice = remaining × `--decay-factor` until < `--min-slice-qty` |
| `weighted` | Custom `--weights 0.5,0.3,0.2` (normalised); count = weight vector length |

**Shared parameters** (all strategies):
```
--target-qty          total qty per leg in base currency (0 = full level-0 qty)
--min-spread-bps      skip threshold (enforced by ThresholdStrategy wrapper)
```

**Strategy-specific parameters:**
```
# ThresholdStrategy
--min-spread-bps N        minimum spread to trade

# SliceStrategy
--slice-mode equal|decay|weighted
--split-count N           number of slices (equal mode, default 4)
--slice-delay-us US       µs between consecutive slices (default 0)
--decay-factor F          fraction of remaining qty per slice (decay mode, default 0.5)
--min-slice-qty Q         stop slicing when residual < Q (decay mode, default 0.001)
--weights W1,W2,...       normalised weight vector (weighted mode)
```

Adding a new strategy requires only a new class implementing `IExecStrategy` and
one line in `exec.cpp`'s factory — no changes to `ExecEngine`.

### `OrderTracker` (E5)
```cpp
// Pending order entry
struct Pending {
    Order order;
    boost::asio::steady_timer deadline;
    bool confirmed{false};
};
```
- On `submit_order()` → adds entry, arms timer at `--confirm-timeout-ms`
- On fill callback → marks `confirmed=true`, cancels timer
- On timer expiry → logs `[EXEC] CONFIRMATION TIMEOUT order_id=...`
  → calls `IOrderClient::submit_order()` with opposing hedge (at market)
  → updates position tracking accordingly

### `ExecEngine` state
```
std::string my_venue_
std::atomic<bool> paused_{false}        // E2 kill switch
double open_notional_{0.0}             // E1 position tracking (updated on fill)
std::unordered_map<std::string, int64_t> last_signal_ns_   // E3 dedup
double position_limit_{0.0}            // E1 config (0 = no limit)
double max_order_notional_{0.0}        // E4 config (0 = no limit)
int64_t cooldown_ns_{0}               // E3 config
int64_t confirm_timeout_ms_{500}       // E5 config
```

### `exec` CLI (`ExecCmdLine.hpp`)
```
--venue                    required  (e.g. "binance")
--brain-signal-host        required  (brain hostname)
--brain-signal-port        required  (e.g. 8444)
--brain-signal-insecure    (dev: skip TLS cert check)
--certfile / --keyfile     (mTLS client cert)
--position-limit           (E1: max open notional, 0=off)
--max-order-notional       (E4: fat-finger ceiling, 0=off)
--cooldown-ms              (E3: dedup window, 0=off)
--confirm-timeout-ms       (E5: hedge trigger, default 500)
--strategy                 (immediate|threshold|slice, default immediate)
--target-qty               (max qty per leg in base currency, 0=full level-0, default 0)
--min-spread-bps           (ThresholdStrategy: minimum spread to act, default 0)
--slice-mode               (SliceStrategy: equal|decay|weighted, default equal)
--split-count N            (SliceStrategy/equal: number of slices, default 4)
--slice-delay-us US        (SliceStrategy: µs between slices, default 0)
--decay-factor F           (SliceStrategy/decay: fraction per step, default 0.5)
--min-slice-qty Q          (SliceStrategy/decay: stop threshold, default 0.001)
--weights W1,W2,...        (SliceStrategy/weighted: normalised weight vector)
--log-level
--config                   (config file, same ini format as pop/brain)
```

---

## Files to Create (new)

| File | Lines | Purpose |
|---|---|---|
| `brain/include/brain/SignalServer.hpp` | ~60 | Outbound WS server, broadcast API |
| `brain/src/brain/SignalServer.cpp` | ~150 | Accept loop, session outbox, mTLS |
| `exec/CMakeLists.txt` | ~20 | Links common_core, pop_core (for RestClient) |
| `exec/include/exec/IOrderClient.hpp` | ~30 | Order / Fill structs + pure interface |
| `exec/include/exec/StubOrderClient.hpp` | ~20 | Stub: logs + immediate synthetic fill |
| `exec/include/exec/IExecStrategy.hpp` | ~25 | Pure `IExecStrategy` interface (on_signal, pause, resume) |
| `exec/include/exec/ImmediateStrategy.hpp` | ~30 | Single market order, level-0 qty |
| `exec/include/exec/ThresholdStrategy.hpp` | ~25 | Spread-filter wrapper around any inner strategy |
| `exec/include/exec/SliceStrategy.hpp` | ~45 | Sliced execution: equal / decay / weighted sizing modes |
| `exec/src/exec/Strategies.cpp` | ~150 | Implementations of ImmediateStrategy, ThresholdStrategy, SliceStrategy |
| `exec/include/exec/ExecCmdLine.hpp` | ~70 | Boost.program_options CLI (all strategy + guard flags) |
| `exec/include/exec/OrderTracker.hpp` | ~40 | E5: Asio timer table |
| `exec/include/exec/ExecEngine.hpp` | ~40 | Orchestrator header |
| `exec/src/exec/ExecEngine.cpp` | ~120 | E1–E5 guards + `strategy_->on_signal()` dispatch |
| `exec/src/exec/OrderTracker.cpp` | ~60 | Timer arm/cancel/expiry/hedge |
| `exec/app/exec.cpp` | ~80 | main(): WsClient → ExecEngine |

## Files to Modify (existing)

| File | Change |
|---|---|
| `brain/include/brain/ArbDetector.hpp` | Add `std::function<void(const ArbCross &)> on_cross_` |
| `brain/src/brain/ArbDetector.cpp` | Invoke `on_cross_` in `emit_()` |
| `brain/include/brain/BrainCmdLine.hpp` | Add `--signal-port`, `--signal-certfile/keyfile/ca-certfile` |
| `brain/app/brain.cpp` | Instantiate `SignalServer`, wire `on_cross_` callback |
| `CMakeLists.txt` | Add `add_subdirectory(exec)` |

---

## Implementation Order

1. `ArbDetector` callback hook (2 lines — unblocks everything else)
2. `SignalServer` (brain outbound WS push)
3. Wire `SignalServer` in `brain.cpp` + `BrainCmdLine`
4. `IOrderClient` + `StubOrderClient`
5. `IExecStrategy` interface + `ImmediateStrategy` + `ThresholdStrategy` + `SliceStrategy`
6. `OrderTracker` (E5 timer logic)
7. `ExecEngine` (E1–E4 guards + `strategy_->on_signal()` dispatch)
7. `exec.cpp` main + `ExecCmdLine`
8. `exec/CMakeLists.txt` + root `CMakeLists.txt`

---

## Critical Existing Code to Reuse

| Component | File | Reuse |
|---|---|---|
| `WsClient` | `common/include/connection_handler/WsClient.hpp` | exec connects to brain SignalServer |
| `RestClient` | `pop/include/connection_handler/RestClient.hpp` | future real exchange adapters |
| `WsServer`/`WsSession` | `brain/src/brain/WsServer.cpp` | Pattern for SignalServer accept loop |
| `md::log::init()` | `common/include/utils/Log.hpp` | logging in exec |
| Boost.program_options | `brain/include/brain/BrainCmdLine.hpp` | Pattern for ExecCmdLine |

---

## Verification

```bash
# Build
cmake --build build -j4 --target brain zidabot_replay exec

# Generate certs (if using mTLS on signal channel)
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout /tmp/signal_key.pem -out /tmp/signal_cert.pem -subj "/CN=localhost"

# Start brain with signal port enabled
./build/brain/brain --config config/brain.conf \
  --signal-port 8444 \
  --signal-certfile /tmp/signal_cert.pem \
  --signal-keyfile  /tmp/signal_key.pem

# Start exec for binance (in another terminal)
./build/exec/exec \
  --venue binance \
  --brain-signal-host 127.0.0.1 \
  --brain-signal-port 8444 \
  --brain-signal-insecure \
  --position-limit 10000 \
  --max-order-notional 500 \
  --cooldown-ms 1000 \
  --confirm-timeout-ms 500

# Start PoP instances to generate real signals
./build/pop/pop --config config/binance.conf
./build/pop/pop --config config/okx.conf

# Expected log in exec when a cross fires:
# [EXEC] SIGNAL sell=binance buy=okx spread=1.2bps → SELL 0.1 BTC @ 95200 [stub]
# [EXEC] ORDER SUBMITTED client_id=abc123 [stub fill: 0.1 BTC @ 95200]
# [EXEC] FILL confirmed client_id=abc123 qty=0.1 lag=3ms

# Kill switch test
kill -USR1 $(pgrep exec)
# Expected: [EXEC] kill switch ACTIVATED — order submission paused
```
