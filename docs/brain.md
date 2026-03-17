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
| `--certfile` | — | **Yes** | TLS certificate PEM file |
| `--keyfile` | — | **Yes** | TLS private key PEM file |
| `--output` | — | No | Arb signal output JSONL file path |
| `--min-spread-bps` | `0.0` | No | Minimum spread in bps to emit (0 = all crosses) |
| `--max-age-ms` | `5000` | No | Max book-age difference (ms) for staleness guard |
| `--depth` | `50` | No | Order book depth per venue |

**Generating a self-signed cert for local dev:**

```bash
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout /tmp/brain_key.pem -out /tmp/brain_cert.pem -subj "/CN=localhost"
```

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
    Level                    parse_level            (const nlohmann::json &lvl);
    GenericSnapshotFormat    parse_snapshot         (const nlohmann::json &j);
    GenericIncrementalFormat parse_incremental      (const nlohmann::json &j);
    GenericSnapshotFormat    parse_book_state_as_snapshot(const nlohmann::json &j);
    int64_t                  extract_ts_book_ns     (const nlohmann::json &j);
}
```

`priceTick` and `quantityLot` are read directly as integers from the JSON (pre-computed by PoP); no `parsePriceToTicks` conversion is needed here. `parse_book_state_as_snapshot` maps `applied_seq` to `lastUpdateId` so the result can be passed to `onSnapshot(WsAuthoritative)`.

`parse_header` enforces `schema_version == 1` and throws `std::runtime_error` if a different version is received. This prevents silent corruption if a future PoP version changes the wire format.

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
    ArbDetector(double min_spread_bps, int64_t max_age_diff_ns, std::string output_path);
    std::vector<ArbCross> scan(const std::vector<VenueBook> &venues);
    void flush() noexcept;  // flush output file; call before shutdown
};
```

**Cross condition**

For every directed pair `(sell, buy)` where `sell != buy`:

1. Both books must pass `synced()` (i.e. `feed_healthy && controller->isSynced()`).
2. Neither `best_bid.priceTick` nor `best_ask.priceTick` may be zero (empty sentinel).
3. Cross: `sell.best_bid.priceTick > buy.best_ask.priceTick`.
4. **Individual book age**: `now - sell.ts_book_ns <= max_age_diff_ns` AND `now - buy.ts_book_ns <= max_age_diff_ns` — catches two books both stale by the same absolute amount.
5. **Relative staleness**: `|sell.ts_book_ns - buy.ts_book_ns| <= max_age_diff_ns` — catches books that diverged in time from each other.

```
mid        = (sell_bid_tick + buy_ask_tick) / 2   (skipped if mid <= 0)
spread_bps = ((sell_bid_tick - buy_ask_tick) / mid) × 10 000
```

Since `priceTick = price × 100` uniformly across all venues, the ratio is dimensionless and `spread_bps` is correct without any additional conversion.

**Output**

Each detected cross is:
- Printed to stderr.
- Appended as a JSON line to `--output` if configured.
- `flush()` is called in the SIGINT/SIGTERM handler before `ioc.stop()` to prevent partial-line loss on shutdown.

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
# Generate self-signed cert (once)
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout /tmp/brain_key.pem -out /tmp/brain_cert.pem -subj "/CN=localhost"

# Start brain
./build/brain/brain \
  --port 8443 \
  --certfile /tmp/brain_cert.pem \
  --keyfile  /tmp/brain_key.pem \
  --output   /tmp/arb.jsonl \
  --min-spread-bps 0 \
  --depth 50

# Start PoP (in separate terminals, one per venue)
./build/pop/pop --venue binance --base BTC --quote USDT \
  --brain_ws_host 127.0.0.1 --brain_ws_port 8443 --brain_ws_path / \
  --brain_ws_insecure \
  --persist_book_every_updates 1000 --persist_book_top 50

./build/pop/pop --venue okx --base BTC --quote USDT \
  --brain_ws_host 127.0.0.1 --brain_ws_port 8443 --brain_ws_path / \
  --brain_ws_insecure \
  --persist_book_every_updates 1000 --persist_book_top 50
```

Arb crosses appear on brain's stderr and in `/tmp/arb.jsonl`.
