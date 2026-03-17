# pop — Market Data Ingestion Process

`pop/` is a single-venue, single-symbol market data ingestion edge process. Run one instance per venue. It connects to an exchange, maintains a live order book, and fans out normalized events to persistence and/or the brain.

```
./build/pop/pop --venue binance --base BTC --quote USDT [options]
```

---

## Command-line reference

Parsed by `pop/include/CmdLine.hpp` into `CmdOptions`.

### Required

| Flag | Description |
|---|---|
| `--venue` | Venue name: `binance`, `okx`, `bybit`, `bitget`, `kucoin` |
| `--base` | Base asset, e.g. `BTC` |
| `--quote` | Quote asset, e.g. `USDT` |

### Order book

| Flag | Default | Description |
|---|---|---|
| `--depthLevel` | 400 | Local order book depth per side |

### Endpoint overrides (optional)

All default to venue-canonical values. Override only for testing or alternate regions.

| Flag | Description |
|---|---|
| `--ws_host / --ws_port / --ws_path` | WebSocket endpoint |
| `--rest_host / --rest_port / --rest_path` | REST snapshot endpoint |

### Brain publishing (optional)

Disabled when `--brain_ws_host` is absent.

| Flag | Default | Description |
|---|---|---|
| `--brain_ws_host` | — | Brain server hostname or IP |
| `--brain_ws_port` | — | Brain server port (e.g. `8443`) |
| `--brain_ws_path` | — | WebSocket path (e.g. `/`) |
| `--brain_ws_insecure` | false | Disable TLS cert verification (local dev only) |

### Persistence (optional)

| Flag | Default | Description |
|---|---|---|
| `--persist_path` | — | Output file path; `.gz` extension enables GZIP compression |
| `--persist_book_every_updates` | 0 | Emit a `book_state` checkpoint every N applied incremental updates (0 = disabled) |
| `--persist_book_top` | 50 | Levels per side to include in `book_state` |

### Network tuning (optional)

| Flag | Default | Description |
|---|---|---|
| `--rest_timeout_ms` | 8000 | REST snapshot request timeout in milliseconds (minimum 1000) |

### Logging

| Flag | Default | Description |
|---|---|---|
| `--log_path` | — | Redirect process log to file (`.log` appended if no extension) |

### Debug flags

| Flag | Default | Description |
|---|---|---|
| `--debug` | false | Enable rate-limited debug logging |
| `--debug_raw` | false | Print truncated raw WS frames |
| `--debug_every` | 200 | Print one debug line per N parsed messages |
| `--debug_raw_max` | 512 | Max characters of raw message to print |
| `--debug_top` | 3 | Top N levels to print in debug output |
| `--debug_no_checksum` | false | Omit checksum fields from debug lines |
| `--debug_no_seq` | false | Omit sequence fields from debug lines |

---

## Architecture

### `FeedHandlerConfig` (`pop/include/abstract/FeedHandler.hpp`)

Carries the full configuration for one feed instance. `main.cpp` populates this from `CmdOptions` and passes it to `GenericFeedHandler::init()`.

Key fields:

```cpp
VenueId     venue_name;
std::string symbol, base_ccy, quote_ccy;
std::string ws_host, ws_port, ws_path;    // "" = use venue default
std::string rest_host, rest_port, rest_path;
std::string brain_ws_host, brain_ws_port, brain_ws_path;
bool        brain_ws_insecure;
std::string persist_path;
std::size_t persist_book_every_updates;
std::size_t persist_book_top;
int         rest_timeout_ms;              // REST snapshot timeout (default 8000 ms)
std::size_t depthLevel;
```

### `IVenueFeedHandler` (`pop/include/abstract/FeedHandler.hpp`)

Minimal lifecycle interface:

```cpp
FeedOpResult init(const FeedHandlerConfig &);  // validate + setup, no I/O
FeedOpResult start();                           // enqueue async ops
FeedOpResult stop();                            // close sockets, cancel timers
```

### `GenericFeedHandler` (`pop/include/md/GenericFeedHandler.hpp`)

Concrete implementation of `IVenueFeedHandler`. Owns all runtime state for one feed.

**Sync state machine**

```
DISCONNECTED
   │ start() / restartSync()
   ▼
CONNECTING
   │ WS open
   ▼
BOOTSTRAPPING          (KuCoin: HTTP bullet fetch before WS connect)
   │
   ├── RestAnchored venues (Binance, OKX, Bybit, Bitget):
   │      WAIT_REST_SNAPSHOT  (WS open, incrementals buffered, REST in flight)
   │         │ REST response
   │         ▼
   │      WAIT_BRIDGE  (snapshot loaded, draining buffer until bridging update)
   │         │ bridging incremental applied
   │         ▼
   │      SYNCED
   │
   └── WsAuthoritative venues (KuCoin):
          WAIT_WS_SNAPSHOT  (waiting for WS snapshot message)
             │ snapshot received
             ▼
          SYNCED
```

**Resync triggers**
- `OrderBookController` returns `NeedResync` (sequence gap or checksum failure)
- Watchdog timer fires (no WS messages for `ws_stale_after_ms`)
- WS disconnect

On any trigger, `restartSync()` resets to `DISCONNECTED` and schedules a reconnect with exponential backoff.

**Owned components**

| Member | Type | Role |
|---|---|---|
| `ws_` | `WsClient` | Exchange WebSocket |
| `rest_` | `RestClient` | REST snapshot fetch |
| `controller_` | `OrderBookController` | Book state machine |
| `persist_` | `FilePersistSink` | JSONL.gz writer (optional) |
| `brain_publish_` | `WsPublishSink` | Brain publisher (optional) |
| `adapter_` | `AnyAdapter` (variant) | Venue-specific parsing |
| `buffer_` | `deque<BufferedMsg>` | Incrementals buffered before sync, max 10 000 |

---

### `VenueAdapter` (`pop/include/md/VenueAdapter.hpp`)

`std::variant` over five concrete adapters: `BinanceAdapter`, `OKXAdapter`, `BybitAdapter`, `BitgetAdapter`, `KucoinAdapter`.

Each adapter implements:

| Method | Path | Description |
|---|---|---|
| `caps()` | cold | Returns `VenueCaps` (sync mode, checksum policy, etc.) |
| `wsEndpoint(cfg)` | cold | Resolve WS host/port/target |
| `restEndpoint(cfg)` | cold | Resolve REST host/port |
| `wsSubscribeFrame(cfg)` | cold | Build subscribe JSON string |
| `restSnapshotTarget(cfg)` | cold | Build REST path with symbol + depth |
| `parseWsMessage(data, len, ...)` | **hot** | Parse raw WS bytes → snapshot or incremental |

**`VenueCaps`** encodes per-venue behavior:

```cpp
struct VenueCaps {
    SyncMode sync_mode;          // RestAnchored or WsAuthoritative
    bool ws_sends_snapshot;      // WS delivers snapshot frames
    bool has_checksum;
    bool requires_ws_bootstrap;  // KuCoin: needs HTTP bullet first
    bool allow_seq_gap;          // tolerate non-contiguous sequences
    ChecksumFn checksum_fn;
    uint8_t checksum_top_n;
};
```

**Sync modes by venue**

| Venue | SyncMode | Checksum | Notes |
|---|---|---|---|
| Binance | RestAnchored | No | Standard REST+WS bridge |
| OKX | RestAnchored | No | |
| Bybit | RestAnchored | No | |
| Bitget | RestAnchored | CRC-32 (top-25) | |
| KuCoin | WsAuthoritative | No | HTTP bullet bootstrap required; `allow_seq_gap=true` |

---

### `FilePersistSink` (`pop/include/postprocess/FilePersistSink.hpp`)

Writes normalized events as JSONL, optionally GZIP-compressed (`.gz` extension).

```cpp
FilePersistSink(path, venue, symbol);
void write_snapshot  (snap, source);
void write_incremental(inc, source);
void write_book_state(book, applied_seq, top_n, source, ts_book_ns);
```

`persist_seq_` is an independent monotonic counter; it is not shared with `WsPublishSink`. Each record includes `persist_seq`, `ts_persist_ns`, `venue`, and `symbol`. See [PERSISTENCE_SCHEMA.md](PERSISTENCE_SCHEMA.md) for the full schema.

---

### `WsPublishSink` (`pop/include/postprocess/WsPublishSink.hpp`)

Publishes the same normalized events as individual WebSocket text frames to the brain server. Payload schema is identical to `FilePersistSink`.

```cpp
WsPublishSink(ioc, host, port, target, insecure_tls, venue, symbol);
void start();
void stop();
void publish_snapshot   (snap, source);
void publish_incremental(inc, source);
void publish_book_state (book, applied_seq, top_n, source, ts_book_ns);
void publish_status     (feed_state, reason);  // called by GenericFeedHandler on every state transition
```

`publish_status` emits a `"status"` event so brain can immediately invalidate or re-trust the book on feed state changes. `feed_state` is one of `"disconnected"`, `"resyncing"`, `"synced"`. See [PERSISTENCE_SCHEMA.md](PERSISTENCE_SCHEMA.md) for the full status event schema.

**Reconnect behavior**: exponential backoff, starting at 1 s, capped at 30 s. The reconnect timer generation prevents stale callbacks from firing after a stop/restart cycle.

**Backpressure**: messages are queued in `WsClient`'s outbox (default cap 5 000). When full, the oldest messages are dropped (freshness over completeness).

**TLS**: peer verification enabled by default. Pass `insecure_tls=true` only for local dev with self-signed certs; a warning is printed on every connection.

---

### `RestClient` (`pop/include/connection_handler/RestClient.hpp`)

Async HTTPS GET client (Boost.Beast, TLS). Used exclusively by `GenericFeedHandler::requestSnapshot()`. Single-shot: make one request and call the completion handler.

---

## Data flow summary

```
Exchange WS → WsClient::on_raw_message
                  │
                  ▼
             VenueAdapter::parseWsMessage
                  │ GenericSnapshotFormat / GenericIncrementalFormat
                  ▼
             OrderBookController::onSnapshot / onIncrement
                  │ Action::None (keep going) or NeedResync (restart)
                  │
                  ├─── FilePersistSink::write_*      (if persist_path set)
                  └─── WsPublishSink::publish_*       (if brain_ws_host set)
```
