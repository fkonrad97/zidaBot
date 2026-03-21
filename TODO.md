# Trading Engine — TODO & Progress Tracker

> Update this file as work is completed. Statuses: ✅ Done · 🔄 In Progress · ⬜ Not Started

---

## Track A — Correctness & Security Fixes

All items in this track are complete.

| # | Item | Status | Files | Notes |
|---|---|---|---|---|
| A1 | Binary hardening flags in CMake (`-fstack-protector-strong`, `-fPIE`, RELRO, ASAN/TSAN options, nlohmann/json hash pin) | ✅ Done | `CMakeLists.txt` | Verified via `readelf` — BIND_NOW + PIE confirmed |
| A2 | Replace `assert(depth>0)` with `throw std::invalid_argument` in OrderBook | ✅ Done | `common/include/orderbook/OrderBook.hpp` | |
| A3 | Integer-only price parsing (`std::from_chars`, no `stod` double intermediate) | ✅ Done | `common/include/orderbook/OrderBookUtils.hpp` | Safe up to ~9×10¹⁴ USD |
| A4 | TLS 1.2+ floor + ECDHE-only cipher list — client side | ✅ Done | `common/src/connection_handler/WsClient.cpp` | |
| A4 | TLS 1.2+ floor + ECDHE-only cipher list — server side | ✅ Done | `brain/app/brain.cpp` | |
| A5 | Replace manual X509 callback with `ssl::rfc2818_verification` (full chain validation) | ✅ Done | `common/src/connection_handler/WsClient.cpp` | Full chain validated, not just leaf cert |
| A6 | WsServer: 4 MB max frame size + 10-connection cap | ✅ Done | `brain/src/brain/WsServer.cpp` | |
| A7 | ArbDetector: individual book age check (not just relative diff) | ✅ Done | `brain/src/brain/ArbDetector.cpp` | Both absolute + relative staleness guards |
| A8 | ArbDetector: `mid <= 0` guard before spread_bps calculation | ✅ Done | `brain/src/brain/ArbDetector.cpp` | |
| A9 | brain.cpp: overflow-safe `max_age_ms * 1_000_000LL` cast | ✅ Done | `brain/app/brain.cpp` | |
| A10 | JsonParsers: throw on `schema_version != 1` | ✅ Done | `brain/include/brain/JsonParsers.hpp` | |
| A11 | UnifiedBook: key on `(venue, symbol)` not just venue | ✅ Done | `brain/src/brain/UnifiedBook.cpp` | Supports multi-symbol from same venue |
| A12 | REST timeout configurable via `--rest_timeout_ms` (default 8 s) | ✅ Done | `pop/include/CmdLine.hpp`, `pop/src/md/GenericFeedHandler.cpp` | |
| A13 | ArbDetector: `flush()` called in signal handler before `ioc.stop()` | ✅ Done | `brain/app/brain.cpp`, `brain/src/brain/ArbDetector.cpp` | |
| A14 | WsServer: eager prune of expired `weak_ptr` sessions in `stop()` | ✅ Done | `brain/src/brain/WsServer.cpp` | |
| A+ | Feed health: PoP publishes `status` events; brain tracks `feed_healthy` per VenueBook; `synced()` requires both `feed_healthy` && `isSynced()` | ✅ Done | `pop/src/postprocess/WsPublishSink.cpp`, `pop/src/md/GenericFeedHandler.cpp`, `brain/src/brain/UnifiedBook.cpp` | Prevents stale-book arb on PoP disconnect |

---

## Track B — Safety & Emergency Controls

Circuit breakers and data guards to prevent runaway signals or bad data from causing harm.

| # | Item | Priority | Status | Files | Notes |
|---|---|---|---|---|---|
| B1 | Max spread cap: configurable `--max-spread-bps` upper bound; crosses above threshold logged as anomalies and suppressed | HIGH | ✅ Done | `brain/src/brain/ArbDetector.cpp`, `brain/include/brain/BrainCmdLine.hpp` | Logs `[ArbDetector] ANOMALY` and skips; 0 = no cap |
| B2 | Arb signal rate limiter: per-pair token bucket (e.g. max 1 signal/sec per sell+buy pair) to prevent output saturation | HIGH | ✅ Done | `brain/src/brain/ArbDetector.cpp`, `brain/include/brain/BrainCmdLine.hpp` | `--rate-limit-ms` (default 1000 ms); keyed by "sell:buy" |
| B3 | Tick sanity bounds: reject levels with `priceTick <= 0` or `quantityLot < 0`; log and discard | HIGH | ✅ Done | `brain/include/brain/JsonParsers.hpp` | Throws `std::runtime_error` on bad levels; caught by `UnifiedBook::on_event` |
| B4 | Timestamp validation: reject `ts_book_ns` more than N seconds in the past or future vs. system clock | HIGH | ✅ Done | `brain/src/brain/UnifiedBook.cpp` | Future ts (>60 s) clamped to now; past ts (>5 min) logs WARN |
| B5 | Cross-venue price sanity: if best_bid on any venue deviates > configurable % from the median across all venues, log anomaly and skip that venue in arb scan | MEDIUM | ✅ Done | `brain/src/brain/ArbDetector.cpp`, `brain/include/brain/BrainCmdLine.hpp` | `--max-price-deviation-pct` (0=disabled); median computed each scan over synced venues |
| B6 | GenericFeedHandler reconnect: exponential backoff (start 1 s, max 60 s) with ±25% jitter; currently fixed 200 ms | MEDIUM | ✅ Done | `pop/src/md/GenericFeedHandler.cpp`, `pop/include/md/GenericFeedHandler.hpp` | Resets to 1 s on successful SYNCED transition |
| B7 | Quantity sanity: reject or warn on levels with `quantityLot == 0` that arrive in snapshot (not just incremental remove) | LOW | ✅ Done | `common/src/orderbook/OrderBookController.cpp` | Zero-qty levels logged + skipped in `onSnapshot` before sort/apply |

---

## Track C — Data Quality

Integrity checks to catch corrupted or implausible book state before it drives signals.

| # | Item | Priority | Status | Files | Notes |
|---|---|---|---|---|---|
| C1 | Book crossing check: after each incremental, verify `best_bid.priceTick < best_ask.priceTick`; trigger resync if crossed | HIGH | ✅ Done | `common/src/orderbook/OrderBookController.cpp` | Checked after both bridge and steady-state incrementals; triggers `need_resync_("book_crossed")` |
| C2 | Per-venue update rate monitor: track messages/sec; log WARN if rate exceeds configurable ceiling (e.g. 2×expected) | MEDIUM | ✅ Done | `pop/src/md/GenericFeedHandler.cpp`, `pop/include/abstract/FeedHandler.hpp`, `pop/include/CmdLine.hpp` | `--max-msg-rate N`; rate computed at heartbeat (60 s), WARN if > 2×ceiling |
| C3 | Periodic `OrderBook::validate()`: call every N updates in debug / staging builds to catch sort-order violations | MEDIUM | ✅ Done | `common/include/orderbook/OrderBookController.hpp`, `common/src/orderbook/OrderBookController.cpp` | `--validate-every N` (0=off); triggers `need_resync_("validate_failed")` on violation |
| C4 | REST snapshot staleness warning: log WARN if `lastUpdateId` from REST is more than M seconds behind the live WS stream | MEDIUM | ✅ Done | `pop/src/md/GenericFeedHandler.cpp`, `pop/include/md/GenericFeedHandler.hpp` | Logs REST latency ms and buffer depth on each snapshot response |
| C5 | Incremental checksum coverage: document which venues lack checksums; add a `--require-checksum` strict mode that triggers resync on any unverified incremental | LOW | ✅ Done | `common/include/orderbook/OrderBookController.hpp`, `common/src/orderbook/OrderBookController.cpp`, `pop/include/abstract/FeedHandler.hpp`, `pop/include/CmdLine.hpp`, `pop/src/md/GenericFeedHandler.cpp`, `docs/pop.md` | `setHasChecksum` + `setRequireChecksum` on controller; docs table added to `docs/pop.md`; only OKX currently active |

---

## Track D — Observability & Operations

Needed to run the engine unattended and debug incidents.

| # | Item | Priority | Status | Files | Notes |
|---|---|---|---|---|---|
| D1 | Structured logging: replace free-form `std::cerr` with a levelled, structured log facade (spdlog recommended); log level configurable at runtime | HIGH | ✅ Done | `common/include/utils/Log.hpp`, `common/src/utils/Log.cpp`, all `.cpp` translation units | spdlog v1.14.1; `--log_level` (pop) / `--log-level` (brain); stderr + optional file sink; flush_on(warn) |
| D2 | Per-venue counters: messages received, resyncs triggered, outbox drops, crosses detected — exposed via stderr heartbeat | HIGH | ✅ Done | `pop/src/md/GenericFeedHandler.cpp`, `brain/src/brain/ArbDetector.cpp` | PoP: `[HEARTBEAT]` every 60 s with msgs/book_updates/resyncs; Brain: total crosses on `flush()` |
| D3 | Output file rotation: size- or time-based rotation for `--output` arb JSONL and `--persist_path` files; prevents unbounded disk growth | MEDIUM | ✅ Done | `brain/src/brain/ArbDetector.cpp`, `brain/include/brain/BrainCmdLine.hpp`, `pop/src/postprocess/FilePersistSink.cpp`, `pop/include/postprocess/FilePersistSink.hpp` | `--output-max-mb N` for arb JSONL; persist sink rotates plain JSONL; .gz rotation deferred |
| D4 | Brain watchdog: periodic timer (e.g. 60 s) that logs WARN if `synced_count() == 0` or no arb scan has fired for N seconds | MEDIUM | ✅ Done | `brain/app/brain.cpp`, `brain/include/brain/BrainCmdLine.hpp`, `brain/include/brain/ArbDetector.hpp` | `--watchdog-no-cross-sec N` (0=off); always warns when synced_count==0 |
| D5 | Health endpoint: Unix-socket or HTTP endpoint queryable for current feed status without grepping stderr | LOW | ⬜ Not Started | brain, pop | |
| D6 | Process supervisor config: systemd unit files (or supervisord configs) for auto-restart on crash | LOW | ⬜ Not Started | `deploy/` (new directory) | |

---

## Track E — Execution Layer Safety

Applies only once an order-sending layer is built. Not currently implemented.

| # | Item | Priority | Status | Notes |
|---|---|---|---|---|
| E1 | Position limits: max open notional per venue before new signal legs are blocked | HIGH | ⬜ Future | requires execution adapter |
| E2 | Kill switch: SIGUSR1 pauses signal emission without stopping data collection | HIGH | ⬜ Future | |
| E3 | Signal dedup / cooldown: suppress repeat signals for the same (sell, buy) pair within a configurable window | HIGH | ⬜ Future | `brain/src/brain/ArbDetector.cpp` |
| E4 | Fat-finger guard: max notional per leg must not exceed configured ceiling | HIGH | ⬜ Future | requires execution adapter |
| E5 | Confirmation timeout + hedge: if a fill is not confirmed within N ms, trigger opposing hedge | MEDIUM | ⬜ Future | requires execution adapter |

---

## Track F — Production Architecture

Larger architectural changes for running at scale or in a high-availability setup.

| # | Item | Priority | Status | Notes |
|---|---|---|---|---|
| F1 | Mutual TLS (mTLS) between PoP and Brain: PoP presents client cert signed by private CA; brain verifies it | HIGH | ✅ Done | `common/include/connection_handler/WsClient.hpp`, `common/src/connection_handler/WsClient.cpp`, `pop/include/postprocess/WsPublishSink.hpp`, `pop/src/postprocess/WsPublishSink.cpp`, `pop/include/abstract/FeedHandler.hpp`, `pop/include/CmdLine.hpp`, `pop/app/main.cpp`, `pop/src/md/GenericFeedHandler.cpp`, `brain/include/brain/BrainCmdLine.hpp`, `brain/app/brain.cpp` | `--brain_ws_certfile/keyfile` on PoP; `--ca-certfile` on brain; opt-in, backward-compatible |
| F2 | Configuration file support (YAML/TOML) with CLI overrides; eliminates long command lines for multi-venue deployments | MEDIUM | ✅ Done | `pop/include/CmdLine.hpp`, `brain/include/brain/BrainCmdLine.hpp` | `--config FILE` (key=value format, boost `parse_config_file`); no new deps; CLI overrides file |
| F3 | Multi-symbol support: one PoP process can track multiple symbols; `FeedHandlerConfig` becomes a symbol list | MEDIUM | ✅ Done | `pop/include/CmdLine.hpp`, `pop/app/main.cpp` | `--symbols BTC/USDT,ETH/USDT`; N handlers share one `io_context`; per-symbol persist path derivation; fully backward-compatible with `--base`/`--quote` |
| F4 | Brain active-passive failover: secondary brain subscribes to same PoPs, promotes on primary failure | LOW | ⬜ Not Started | new component |
| F5 | Latency pipeline: histogram (p50/p95/p99) of PoP-receive → brain-detected latency per venue pair | LOW | ⬜ Not Started | `brain/src/brain/ArbDetector.cpp` |
| F6 | Reconnect jitter: ±25% random jitter on all reconnect delays to prevent thundering herd on simultaneous restarts | LOW | ✅ Done | `pop/src/postprocess/WsPublishSink.cpp` | WsPublishSink now uses `mt19937` ±25% jitter matching GenericFeedHandler |

---

## Track G — Concurrency & Scaling

Scaling paths for when symbol count grows or per-message CPU cost increases.
Current single-threaded `io_context` is correct and sufficient for 2–10 symbols (I/O-bound workload).

| # | Item | Priority | Status | Notes |
|---|---|---|---|---|
| G1 | Per-handler thread isolation: one `io_context` + one thread per `GenericFeedHandler`; eliminates single-thread bottleneck for 20+ symbols without strand refactoring | MEDIUM | ⬜ Not Started | Preferred scaling path; each handler fully isolated; join all threads on exit |
| G2 | Thread-pool `io_context`: call `ioc.run()` from N threads to parallelise handlers; requires strand protection on `GenericFeedHandler` internal state (`state_`, `buffer_`, counters) | LOW | ⬜ Not Started | `WsClient`/`RestClient` already strand-safe; `GenericFeedHandler` is not yet |
| G3 | CPU offload: move JSON parsing off the I/O thread onto a `thread_pool`; post results back via `dispatch()`; eliminates parse latency without touching handler state model | LOW | ⬜ Not Started | Worthwhile if symbol count grows beyond ~20 or heavier per-message analytics are added |

---

## How to update this file

- When starting work: change `⬜ Not Started` → `🔄 In Progress`
- When complete: change `🔄 In Progress` → `✅ Done`
- Add new items as they are identified, in the appropriate track
- Do not delete completed rows — they serve as a change history

---

## Batch 6 — ✅ Complete (2026-03-21)

WsClient/RestClient bug fixes (4 bugs) + F3 multi-symbol PoP support.

| # | Item | Status |
|---|---|---|
| Bug 1 | WsClient: missing `closing_` guard in `do_ws_handshake_()` callback (race: could set `opened_=true` on a connection that should tear down) | ✅ Done |
| Bug 2 | WsClient: duplicate state reset in `connect()` — `opened_=false` and `close_notified_.store(false)` were set twice | ✅ Done |
| Bug 3 | WsClient: `set_client_cert()` raw OpenSSL calls (`SSL_use_certificate_chain_file`, `SSL_use_PrivateKey_file`) had no return-value check; bad cert/key silently ignored | ✅ Done |
| Bug 4 | RestClient: HTTP non-2xx responses used `errc::protocol_error` (EPROTO); replaced with custom `http_errc::non_2xx` category | ✅ Done |
| F3 | Multi-symbol: `--symbols BTC/USDT,ETH/USDT`; N `GenericFeedHandler`s share one `io_context`; per-symbol persist path derivation; backward-compatible | ✅ Done |

---

## Batch 5 — ✅ Complete (2026-03-21)

First full end-to-end run on macOS + Boost 1.88. Three platform-compatibility fixes and one latent mTLS bug fixed. No new features; no TODO items changed status.

| # | Item | Status |
|---|---|---|
| — | `ssl::rfc2818_verification` → `ssl::host_name_verification` (removed in Boost 1.88) | ✅ Done |
| — | CMakeLists: guard RELRO/PIE linker flags with `if(NOT APPLE)` | ✅ Done |
| — | `timer.cancel(ignored)` → `timer.cancel()` (overload removed in Boost 1.88) | ✅ Done |
| — | mTLS client cert bug: `set_client_cert()` now sets cert on both `ssl_ctx_` and the stream's native SSL handle | ✅ Done |
| — | All 5 PoP configs: `brain_ws_insecure=true` for local dev (CA not in system trust store) | ✅ Done |

---

## Batch 4 — ✅ Complete (2026-03-20)

D1 structured logging, wiring the spdlog facade across all translation units.

| # | Item | Status |
|---|---|---|
| D1 | Structured logging — spdlog v1.14.1, `--log_level` / `--log-level` CLI flags, cerr → spdlog in all sources | ✅ Done |

---

## Batch 3 — ✅ Complete (2026-03-20)

Scope: close out small correctness gaps, secure the PoP→brain channel, and make the stack operator-friendly with a config file.

| # | Item | Status |
|---|---|---|
| F6 | WsPublishSink reconnect jitter | ✅ Done |
| B7 | Snapshot zero-quantity sanity | ✅ Done |
| C5 | Checksum coverage + `--require_checksum` | ✅ Done |
| F1 | mTLS between PoP and Brain | ✅ Done |
| F2 | Config file support (`--config`, boost ini-style, CLI overrides) | ✅ Done |
