# brain — Central Arbitrage Detection Server

`brain/` is a single central process that accepts inbound TLS WebSocket connections from one or more `pop` instances, maintains a live unified order book per venue, and continuously scans for cross-venue arbitrage opportunities.

```
./build/brain/brain --port 8443 --certfile cert.pem --keyfile key.pem [options]
```

---

## Command-line reference

Parsed by `brain/include/brain/BrainCmdLine.hpp` into `BrainOptions`.

| Flag | Default | Required | Description |
|---|---|---|---|
| `--bind` | `0.0.0.0` | No | Bind address |
| `--port` | `8443` | No | Listen port (WSS) |
| `--certfile` | — | **Yes** | TLS server certificate PEM file |
| `--keyfile` | — | **Yes** | TLS server private key PEM file |
| `--ca-certfile` | — | No | F1: mTLS CA certificate PEM — require PoP clients to present a cert signed by this CA |
| `--output` | — | No | Arb signal output JSONL file path |
| `--min-spread-bps` | `0.0` | No | Only emit crosses at or above this threshold in bps (0 = all) |
| `--max-spread-bps` | `0.0` | No | Suppress crosses above this threshold (0 = no cap); logged as anomalies |
| `--rate-limit-ms` | `1000` | No | Per-pair cross rate limit in ms; repeated signals within window are suppressed |
| `--max-age-ms` | `5000` | No | Max individual book age AND max age difference between the two books (both in ms) |
| `--max-price-deviation-pct` | `0.0` | No | Skip venues whose best_bid deviates more than N% from the median across all synced venues (0 = off) |
| `--depth` | `50` | No | Order book depth per venue |
| `--output-max-mb` | `0` | No | Rotate `--output` file at this size in MB (0 = no rotation); rotated files are renamed `.1`, `.2`, … |
| `--watchdog-no-cross-sec` | `0` | No | Warn on stderr if no arb cross is emitted for this many seconds while ≥ 2 venues are synced (0 = off) |
| `--log-level` | `info` | No | D1: Log verbosity: `debug` \| `info` \| `warn` \| `error` |
| `--health-port` | `0` | No | D5: Plain-HTTP health endpoint port (0 = disabled). `GET /health` returns JSON with synced venue count, per-venue state, last arb cross time, active WS clients, and detection latency percentiles. |
| `--standby` | false | No | F4: Start in passive standby mode — receives and processes all data but emits no arb signals. Promote to active with `kill -USR1 <pid>`. |
| `--config` | — | No | F2: Config file path (key=value per line; CLI flags override file values). See `config/brain.conf` for an example. |

**TLS / mTLS cert setup:** see [docs/HOWTO.md § 3](HOWTO.md#3-tls--mtls-certificate-setup) for full instructions (self-signed dev cert and full mTLS CA setup).

---

## Architecture

### `WsServer` / `WsSession` (`brain/include/brain/WsServer.hpp`)

Async TLS WebSocket server accepting inbound connections from PoP instances.

```
ioc.run()
   │
   ▼
WsServer::do_accept_()  — async_accept loop
   │ new socket
   ▼
WsSession::run()
   ├── do_tls_handshake_()  (server-side TLS)
   ├── do_ws_accept_()      (WS upgrade)
   └── do_read_()           (read loop → on_message callback)
```

- Each accepted connection runs on its own strand (`net::make_strand(ioc)` passed to `async_accept`).
- Brain never writes to PoP; the read loop is one-way only.
- **Connection limit**: at most 10 concurrent sessions are accepted; new connections beyond this are dropped immediately.
- **Max frame size**: `read_message_max(4 MB)` is set per session after the WS handshake to prevent memory exhaustion from oversized frames.
- `WsServer::stop()` prunes expired `weak_ptr` entries then closes all live sessions.
- Sessions are tracked via `weak_ptr`; stale entries are pruned on every new accept and on `stop()`.

**`WsSession` lifecycle**

```
run() → do_tls_handshake_() → do_ws_accept_() → do_read_() loop
```

Each text frame is delivered to the `MessageHandler` callback as `std::string_view`.

---

### `JsonParsers` (`brain/include/brain/JsonParsers.hpp`)

Header-only deserializer for WsPublishSink payloads.

```cpp
namespace brain {
    struct EventHeader {
        int schema_version;
        std::string event_type;  // "snapshot" | "incremental" | "book_state" | "status"
        std::string venue;
        std::string symbol;
    };

    EventHeader              parse_header           (const nlohmann::json &j);  // throws on schema_version != 1
    Level                    parse_level            (const nlohmann::json &lvl);  // throws on priceTick <= 0 or quantityLot < 0
    GenericSnapshotFormat    parse_snapshot         (const nlohmann::json &j);
    GenericIncrementalFormat parse_incremental      (const nlohmann::json &j);
    GenericSnapshotFormat    parse_book_state_as_snapshot(const nlohmann::json &j);
    int64_t                  extract_ts_book_ns     (const nlohmann::json &j);
}
```

`priceTick` and `quantityLot` are read directly as integers from the JSON (pre-computed by PoP); no `parsePriceToTicks` conversion is needed here. `parse_book_state_as_snapshot` maps `applied_seq` to `lastUpdateId` so the result can be passed to `onSnapshot(WsAuthoritative)`.

`parse_header` enforces `schema_version == 1` and throws `std::runtime_error` if a different version is received. This prevents silent corruption if a future PoP version changes the wire format.

`parse_level` validates each level: throws `std::invalid_argument` if `priceTick <= 0` (non-positive price) or `quantityLot < 0` (negative quantity). This catches malformed data from PoP before it can corrupt the book.

---

### `VenueBook` (`brain/include/brain/UnifiedBook.hpp`)

One live order book per `(venue, symbol)` pair, owned by `UnifiedBook`.

```cpp
struct VenueBook {
    std::string venue_name;  // e.g. "binance"
    std::string symbol;      // e.g. "BTCUSDT" — combined with venue_name as the lookup key
    std::unique_ptr<md::OrderBookController> controller;
    std::int64_t ts_book_ns;   // timestamp of last update (for staleness guard)
    bool         feed_healthy; // true only after a data event or "synced" status; cleared on "disconnected"

    bool synced() const noexcept;  // requires feed_healthy && controller->isSynced()
    const md::OrderBook &book() const noexcept;
};
```

`VenueBook` is keyed on `(venue_name, symbol)`. Two PoP instances for the same venue but different symbols each get their own `VenueBook`.

The controller has `setAllowSequenceGap(true)` set (brain may join mid-stream; sequence continuity is not enforceable).

`feed_healthy` is an explicit liveness flag driven by PoP's `status` events. It prevents arb scanning from using a book that was internally `Synced` but whose exchange feed is now down.

**Timestamp sanitization**: `ts_book_ns` values from PoP are passed through `sanitize_ts()` before storage. Timestamps more than 60 s in the future are clamped to now (prevents the staleness guard being permanently bypassed). Timestamps more than 5 min in the past are logged as a warning but kept (so `ArbDetector`'s age guard can reject them normally).

---

### `UnifiedBook` (`brain/include/brain/UnifiedBook.hpp`)

Routes incoming JSON events to the correct `VenueBook`, lazily creating one on first contact.

```cpp
class UnifiedBook {
    std::string on_event(const nlohmann::json &j);  // returns updated venue, or ""
    const std::vector<VenueBook> &venues() const noexcept;
    std::size_t synced_count() const noexcept;
};
```

**Routing logic**

| `event_type` | Action |
|---|---|
| `"snapshot"` | `parse_snapshot` → `onSnapshot(RestAnchored)` → sets `feed_healthy=true` |
| `"book_state"` | `parse_book_state_as_snapshot` → `onSnapshot(WsAuthoritative)` → immediate `Synced` → sets `feed_healthy=true` |
| `"incremental"` | `parse_incremental` → `onIncrement` → sets `feed_healthy=true` |
| `"status"` (feed_state=`"disconnected"`) | sets `feed_healthy=false`, calls `resetBook()`, clears `ts_book_ns` |
| `"status"` (feed_state=`"resyncing"` or unknown) | sets `feed_healthy=false` (book data retained but excluded from arb) |
| `"status"` (feed_state=`"synced"`) | no direct action — subsequent data event restores `feed_healthy` |
| Other / parse error | Discarded; returns `""` |

`book_state` events are treated as authoritative because they represent a fully-materialized book at the PoP side. This allows brain to resync to `Synced` immediately, even when joining mid-stream without a prior REST snapshot.

If `NeedResync` is returned by the controller (unexpected on a stream with `allow_seq_gap=true`), the error is logged to stderr and the book waits for the next `book_state` to recover.

---

### `ArbDetector` (`brain/include/brain/ArbDetector.hpp`)

Scans all directed venue pairs for cross-venue arbitrage. Direct C++ translation of `scripts/unified_arb_stream.py::detect_crosses`.

```cpp
struct ArbCross {
    std::string  sell_venue, buy_venue;
    std::int64_t sell_bid_tick;   // sell venue best bid
    std::int64_t buy_ask_tick;    // buy venue best ask
    double       spread_bps;
    std::int64_t ts_detected_ns;
    std::int64_t sell_ts_book_ns, buy_ts_book_ns;
};

class ArbDetector {
    ArbDetector(double min_spread_bps, double max_spread_bps,
                int64_t rate_limit_ns, int64_t max_age_diff_ns,
                double max_price_deviation_pct,
                std::string output_path, uint64_t output_max_bytes = 0);
    std::vector<ArbCross> scan(const std::vector<VenueBook> &venues);
    void flush() noexcept;                        // flush file + log latency percentiles; call before shutdown
    int64_t last_cross_ns() const;                // D4: timestamp of most recent emitted cross
    void set_active(bool v) noexcept;             // F4: false = standby, suppress emission
    bool is_active() const noexcept;              // F4: current active state
    LatencyHistogram::Percentiles latency_percentiles() const; // F5: p50/p95/p99 in µs
};
```

**Cross condition**

For every directed pair `(sell, buy)` where `sell != buy`:

1. Both books must pass `synced()` (i.e. `feed_healthy && controller->isSynced()`).
2. Neither `best_bid.priceTick` nor `best_ask.priceTick` may be zero (empty sentinel).
3. **Price sanity (B5)**: if `--max-price-deviation-pct > 0`, compute the median `best_bid` across all synced venues; skip any venue whose `best_bid` deviates from the median by more than the configured percent. Anomalies are logged to stderr.
4. Cross: `sell.best_bid.priceTick > buy.best_ask.priceTick`.
5. **Individual book age**: `now - sell.ts_book_ns <= max_age_diff_ns` AND `now - buy.ts_book_ns <= max_age_diff_ns` — catches two books both stale by the same absolute amount.
6. **Relative staleness**: `|sell.ts_book_ns - buy.ts_book_ns| <= max_age_diff_ns` — catches books that diverged in time from each other.

```
mid        = (sell_bid_tick + buy_ask_tick) / 2   (skipped if mid <= 0)
spread_bps = ((sell_bid_tick - buy_ask_tick) / mid) × 10 000
```

Since `priceTick = price × 100` uniformly across all venues, the ratio is dimensionless and `spread_bps` is correct without any additional conversion.

Crosses above `max_spread_bps` (when `> 0`) are logged as anomalies and suppressed. Per-pair rate limiting (`rate_limit_ns`) drops repeated signals for the same `(sell, buy)` pair within the configured window.

**F4 — Standby mode**

When `--standby` is passed, `active_` starts as `false`. `emit_()` is a no-op when inactive —
all guard logic (rate limiting, age checks, price sanity) still runs, but no output is produced.
A `SIGUSR1` handler in `brain.cpp` calls `set_active(true)` atomically; the process immediately
starts emitting from the next scan. No restart is needed. Health endpoint always shows
`"standby": true` while inactive and `"standby": false` after promotion.

**F5 — Detection latency**

`lag_ns = ts_detected_ns − max(sell_ts_book_ns, buy_ts_book_ns)` is recorded per cross into a
`LatencyHistogram` (10 000-sample circular buffer, `brain/include/brain/LatencyHistogram.hpp`).
p50/p95/p99 in µs are exposed in the health endpoint as `"latency_us"` and logged by `flush()`.

**Output**

Each detected cross is:
- Printed to stderr.
- Appended as a JSON line to `--output` if configured. Each line includes `"lag_ns"` (F5).
- When `--output-max-mb > 0`, the output file is rotated at the configured size: the current file is renamed to `.1` (bumping any existing `.1` → `.2`, etc.) and a fresh file is opened.
- `flush()` is called in the SIGINT/SIGTERM handler before `ioc.stop()` to prevent partial-line loss on shutdown. It also logs detection latency percentiles.

---

## Data flow

```
PoP instance A (binance) ──┐
PoP instance B (okx)     ──┤  TLS WebSocket
PoP instance C (bybit)   ──┘
                              │
                              ▼
                         WsServer::on_message(text_frame)
                              │ nlohmann::json parse
                              ▼
                         UnifiedBook::on_event(j)
                              │ routes by event_type + venue
                              ▼
                         VenueBook::controller.onSnapshot / onIncrement
                              │
                         if synced_count >= 2:
                              ▼
                         ArbDetector::scan(venues)
                              │ for each (sell, buy) pair
                              ▼
                         emit ArbCross → stderr + JSONL
```

**Minimum venues to start scanning**: 2 synced venues. The brain begins emitting signals as soon as at least two `VenueBook` instances reach `Synced`.

---

## Running

```bash
# Start brain using config file (recommended — see config/brain.conf)
./build/brain/brain --config config/brain.conf

# Or with explicit flags
./build/brain/brain \
  --port 8443 \
  --certfile /tmp/certs/brain_cert.pem \
  --keyfile  /tmp/certs/brain_key.pem \
  --ca-certfile /tmp/certs/ca_cert.pem \
  --output   /tmp/arb.jsonl \
  --min-spread-bps 0 \
  --depth 50

# Start PoP (in separate terminals, one per venue — see config/*.conf)
./build/pop/pop --config config/binance.conf
./build/pop/pop --config config/okx.conf
```

Arb crosses appear on brain's stderr and in `/tmp/arb.jsonl`.

See [docs/HOWTO.md](HOWTO.md) for the full end-to-end setup guide including mTLS cert generation.
