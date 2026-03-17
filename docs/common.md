# common — Shared C++ Library

`common/` is a static library (`common_core`) shared by both `pop` and `brain`. It contains the order book engine, WebSocket client, and utility helpers. Nothing in `common/` has any dependency on pop- or brain-specific code.

---

## Order Book

### `Level` (global, `OrderBook.hpp`)

```cpp
struct Level {
    std::int64_t priceTick;    // price * 100 (integer ticks)
    std::int64_t quantityLot;  // qty  * 1000 (integer lots)
    std::string  price;        // original string form
    std::string  quantity;
    bool isEmpty() const;      // true when quantityLot == 0
};
enum class Side { BID, ASK };
```

`priceTick` and `quantityLot` are the authoritative numeric fields. String fields are kept for serialization and checksum computation only.

---

### `md::OrderBook` (`common/include/orderbook/OrderBook.hpp`)

Two-sided bounded order book. Bids are stored descending by `priceTick`, asks ascending.

**Construction**

```cpp
explicit OrderBook(std::size_t depth);
```

Reserves `depth + 1` capacity per side to avoid reallocation during insert-then-pop operations.

**Key methods**

| Method | Description |
|---|---|
| `update<Side>(level)` | Apply one level update. If `level.isEmpty()`, delegates to `remove`. Uses `lower_bound` O(log N) for insertion point. Drops levels that do not improve the top-N. |
| `remove<Side>(priceTick)` | Erase a level by price tick. No-op if not found. |
| `best_bid() const` | Top-of-book bid. Returns a static empty sentinel (`priceTick==0`) when the book is empty. |
| `best_ask() const` | Top-of-book ask. Same empty sentinel convention. |
| `bid_ptr(i) const` | Pointer to bid at depth `i`, or `nullptr` if `i` is out of range or the level is empty. |
| `ask_ptr(i) const` | Same for asks. |
| `validate() const` | Asserts depth invariant, strict sort order, and no lingering empty levels. |
| `clear()` | Resets both sides to empty vectors. |

**Performance notes**
- `update<S>` and `remove<S>` are templated; the side comparator is resolved at compile time (two separate template instantiations, no branch on side in the hot path).
- Insert + pop_back avoids reallocation thanks to the `depth + 1` reserve.

---

### Data-transfer structs (`common/include/orderbook/OrderBookController.hpp`)

```cpp
struct GenericSnapshotFormat {
    std::uint64_t lastUpdateId;   // anchor sequence id
    std::int64_t  ts_recv_ns;     // local receive timestamp
    std::int64_t  checksum;
    std::vector<Level> bids, asks;
};

struct GenericIncrementalFormat {
    std::uint64_t first_seq, last_seq, prev_last;
    std::int64_t  ts_recv_ns;
    std::int64_t  checksum;
    std::vector<Level> bids, asks;
};
```

All adapters normalize raw venue messages into one of these two structs before passing to `OrderBookController`.

---

### `md::OrderBookController` (`common/include/orderbook/OrderBookController.hpp`)

State machine that owns an `OrderBook` and enforces sequence continuity.

**State machine**

```
WaitingSnapshot
   │  onSnapshot()
   ▼
WaitingBridge  (RestAnchored only — awaiting bridging incremental)
   │  onIncrement() with matching seq
   ▼
Synced
```

`WsAuthoritative` snapshots skip `WaitingBridge` and land directly in `Synced`.

**Key methods**

| Method | Description |
|---|---|
| `onSnapshot(msg, BaselineKind)` | Bulk-loads the snapshot. `RestAnchored` → `WaitingBridge`; `WsAuthoritative` → `Synced`. |
| `onIncrement(msg)` | Validates `prev_last == last_seq_`, applies deltas. Returns `NeedResync` on a gap. |
| `configureChecksum(fn, topN)` | Attach a checksum validator; called once during adapter init. |
| `setAllowSequenceGap(bool)` | When `true`, non-contiguous sequence increments are accepted (brain mid-stream join, KuCoin). |
| `resetBook()` | Full reset to `WaitingSnapshot`, clears the book and sequence state. |
| `isSynced()` | Returns `true` when state is `Synced`. |
| `book()` | Read-only reference to the inner `OrderBook`. |
| `getAppliedSeqID()` | Last successfully applied `last_seq`. |

**`Action` return value**

```cpp
enum class Action { None, NeedResync };
```

Callers must trigger a full resync when `NeedResync` is returned.

---

## WebSocket Client

### `md::WsClient` (`common/include/connection_handler/WsClient.hpp`)

Async outbound TLS WebSocket client using Boost.Beast on a strand. Used by both `GenericFeedHandler` (exchange feeds) and `WsPublishSink` (PoP→brain).

**Construction** — factory only:

```cpp
static std::shared_ptr<WsClient> create(boost::asio::io_context &ioc);
```

**TLS security** — the constructor enforces:
- Protocol floor: TLS 1.2+ (SSLv2/3 and TLS 1.0/1.1 disabled)
- Cipher suite: ECDHE+AEAD only (`ECDHE-*-AES*-GCM-SHA*`, `ECDHE-*-CHACHA20-POLY1305`)
- Certificate chain validation: Boost.Asio `ssl::rfc2818_verification` (correct RFC 2818 hostname matching for the full chain, not just the leaf certificate)

**Configuration (call before `connect()`)**

| Setter | Default | Description |
|---|---|---|
| `set_on_raw_message(fn)` | — | Called with `(const char*, size_t)` for each received text frame. |
| `set_on_open(fn)` | — | Called once after WebSocket handshake completes. |
| `set_on_close(fn)` | — | Called once on any disconnect (error or graceful). |
| `set_logger(fn)` | — | Diagnostic string log callback. |
| `set_connect_timeout(ms)` | 5 000 ms | Deadline for TCP+TLS+WS handshake. |
| `set_idle_ping(ms)` | 0 (disabled) | App-level WebSocket ping interval. |
| `set_tls_verify_peer(bool)` | `true` | Set to `false` for self-signed certs (dev only). |
| `set_max_outbox(n)` | 10 000 | Bound on queued outgoing messages; oldest dropped on overflow. |

**Lifecycle**

```cpp
void connect(host, port, target);  // begin async chain
void send_text(text);              // enqueue outgoing frame
void close();                      // graceful close
void cancel();                     // hard cancel all async ops
```

All calls are strand-safe; `send_text` may be called from the same strand at any time.

---

## Utilities

### `md::OrderBookUtils` (`common/include/orderbook/OrderBookUtils.hpp`)

```cpp
std::int64_t parseDecimalToScaled(std::string_view s, std::int64_t scale);
std::int64_t parsePriceToTicks   (const std::string &s);  // price * 100
std::int64_t parseQtyToLots      (const std::string &s);  // qty   * 1000
```

Used by venue adapters when converting raw string price/qty fields.

Uses `std::from_chars` integer-only parsing — no `double` intermediate. This is safe for all foreseeable crypto prices (up to ~9 × 10¹⁴ USD at price-tick scale of 100). A `double` intermediate would lose precision above ~9 × 10¹³.

Both `parsePriceToTicks` and `parseQtyToLots` delegate to `parseDecimalToScaled` with the appropriate scale factor. Throws `std::invalid_argument` on malformed input.

---

### `md::CheckSumUtils` (`common/include/utils/CheckSumUtils.hpp`)

```cpp
using ChecksumFn = bool(*)(const OrderBook &, std::int64_t expected, std::size_t topN) noexcept;

uint32_t CRC32Checksum(std::string_view s);
int64_t  CRC32ToSigned(uint32_t u);

bool checkBitgetCRC32(const OrderBook &, int64_t expected, size_t topN) noexcept;
```

`checkBitgetCRC32` interleaves `bid[i].price:bid[i].quantity:ask[i].price:ask[i].quantity` up to `topN` levels, computes CRC-32 (Boost), and compares (as signed int64) to the exchange-provided value. Adapters register their checksum function via `OrderBookController::configureChecksum`.

---

### `md::logging` (`common/include/utils/ProcessLoggingUtils.hpp`)

```cpp
struct ProcessLogSession {
    std::string path;
    std::unique_ptr<std::ostream> stream_owner;
    std::unique_ptr<std::streambuf> log_buf;
};

std::optional<ProcessLogSession>
enable_process_file_logging(const std::string &base_path);
```

Redirects `std::clog` (or the process log stream) to a file. Returns `nullopt` if the file cannot be opened. The `ProcessLogSession` owns the stream; it must be kept alive for the duration of logging.
