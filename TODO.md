# Trading Engine — TODO & Progress Tracker

> Update this file as work is completed. Statuses: ✅ Done · 🔄 In Progress · ⬜ Not Started

---

## Batch 14 — 🔄 In Progress

Execution safety hardening before any live venue rollout. Goal: make the exec path safe to validate on real connections without risking capital.

| # | Item | Status | Files |
|---|---|---|---|
| EX7 | Wire strategy fill callback into `ExecEngine::on_fill()` so E1 position accounting works end-to-end | ✅ Done | `exec/include/exec/ImmediateStrategy.hpp`, `exec/src/exec/Strategies.cpp`, `exec/app/exec.cpp`, `tests/exec/test_exec_pipeline.cpp` |
| EX8 | Harden `venue-fees` parsing: trim whitespace, warn on malformed tokens, reject negative fees, sanity-check config | ✅ Done | `brain/include/brain/BrainCmdLine.hpp`, `tests/brain/test_brain_cmdline.cpp` |
| EX9 | Add paper / dry-run execution mode for venue adapter validation with no live order submission | ✅ Done | `exec/include/exec/DryRunOrderClient.hpp`, `exec/include/exec/ExecCmdLine.hpp`, `exec/app/exec.cpp`, `tests/exec/test_dry_run_order_client.cpp`, `tests/exec/test_exec_cmdline.cpp` |
| EX10 | Add venue rollout controls: arming flag, tiny-notional clamp, one-venue-at-a-time enable gate | ✅ Done | `exec/include/exec/ExecCmdLine.hpp`, `exec/app/exec.cpp`, `tests/exec/test_exec_cmdline.cpp` |
| EX11 | Add safe venue validation flow: unit/replay/testnet/live-disabled docs and checklists | ✅ Done | `docs/HOWTO.md`, `docs/DIAGRAMS.md`, `progress/2026-04-03.md` |

---

## Batch 13 — ⬜ Planned

Execution layer MVP: signal broadcast from brain + exec process with pluggable strategies and E1–E5 safety guards.

| # | Item | Status |
|---|---|---|
| ES1 | `ArbDetector::on_cross_` callback hook (2 lines) | ✅ Done |
| ES2 | `SignalServer` outbound WS push in brain | ✅ Done |
| ES3 | Brain wiring: `BrainCmdLine` signal flags + `brain.cpp` callback | ✅ Done |
| EX1 | `exec/` subproject scaffold + CMakeLists | ✅ Done |
| EX2 | `IOrderClient` interface + `StubOrderClient` | ✅ Done |
| EX3 | `IExecStrategy` interface + `ImmediateStrategy` (MVP strategy) | ✅ Done |
| EX4 | `ExecEngine` with E1–E4 guards | ✅ Done |
| EX5 | `OrderTracker` E5 confirmation timeout + hedge | ✅ Done |
| EX6 | `ExecCmdLine` + `exec.cpp` main | ✅ Done |

Post-MVP strategies (`ThresholdStrategy`, `SliceStrategy`) tracked as EE1/EE2 in Track E.

---

## Bugs & Quick Fixes — ✅ From Code Review (2026-03-30)

Issues found during code review of `feature/exec-layer`. All items fixed 2026-03-31.

### CRITICAL — Thread Safety

| # | Item | Priority | Status | Files |
|---|---|---|---|---|
| CRIT-1 | `SignalServer::sessions_` data race — `broadcast()` runs on scan thread while `on_accept_()` mutates on I/O pool threads; add a server-level `net::strand` and post all `sessions_` access onto it | CRITICAL | ✅ Done | `brain/src/brain/SignalServer.cpp`, `brain/include/brain/SignalServer.hpp` |
| CRIT-2 | `SignalSession::close()` touches raw TCP socket outside session strand while async ops are in flight — dispatch close onto session's strand via `net::post` | CRITICAL | ✅ Done | `brain/src/brain/SignalServer.cpp` |

### HIGH

| # | Item | Priority | Status | Files |
|---|---|---|---|---|
| HIGH-1 | `broadcast()` blocks scan thread while iterating sessions — after CRIT-1 fix, ensure snapshot copy is taken under lock and `send()` called outside lock | HIGH | ✅ Done | `brain/src/brain/SignalServer.cpp` |
| HIGH-2 | `ImmediateStrategy::strand_` stored as dangling reference; `order_seq_` not atomic — store strand by value, use `std::atomic<uint64_t>` | HIGH | ✅ Done | `exec/include/exec/ImmediateStrategy.hpp` |
| HIGH-3 | `StubOrderClient::submit_order` fires callback synchronously — post callback onto strand to match async contract | HIGH | ✅ Done | `exec/include/exec/StubOrderClient.hpp` |
| HIGH-4 | `SignalServer::stopped_` plain `bool` read/written across threads — change to `std::atomic<bool>` | HIGH | ✅ Done | `brain/include/brain/SignalServer.hpp`, `brain/src/brain/SignalServer.cpp` |

### MEDIUM

| # | Item | Priority | Status | Files |
|---|---|---|---|---|
| MED-1 | Hot-path `std::string` heap alloc per venue pair in scan inner loop — pre-allocate and reuse buffer | MEDIUM | ✅ Done | `brain/src/brain/ArbDetector.cpp` |
| MED-2 | Synchronous `j.dump()` + file I/O in `emit_()` on scan thread — defer to background writer thread | MEDIUM | ✅ Done | `brain/src/brain/ArbDetector.cpp`, `brain/include/brain/ArbDetector.hpp` |
| MED-3 | No `read_message_max` on `SignalSession` — add `ws_.read_message_max(65536)` in `do_ws_accept_()` | MEDIUM | ✅ Done | `brain/src/brain/SignalServer.cpp` |
| MED-4 | `on_cross_` assigned after scan thread starts — move assignment before scan thread construction | MEDIUM | ✅ Done | `brain/app/brain.cpp` |
| MED-5 | `SignalServer` lacks server-level strand (root cause of CRIT-1) — add `net::strand strand_` member, bind `do_accept_` and `on_accept_` to it | MEDIUM | ✅ Done | `brain/src/brain/SignalServer.cpp`, `brain/include/brain/SignalServer.hpp` |
| MED-6 | Per-tick heap alloc for `price_anomaly`/`bids` vectors in `scan()` — use `std::array<T, kMaxVenues>` members | MEDIUM | ✅ Done | `brain/src/brain/ArbDetector.cpp`, `brain/include/brain/ArbDetector.hpp` |
| MED-7 | Arb signal JSONL missing `schema_version` + `event_type` fields — add to `emit_()` and `serialize_cross()` | MEDIUM | ✅ Done | `brain/src/brain/ArbDetector.cpp`, `brain/app/brain.cpp` |

### LOW

| # | Item | Priority | Status | Files |
|---|---|---|---|---|
| LOW-1 | `do_accept_` captures `this` raw — use `shared_from_this` | LOW | ✅ Done | `brain/src/brain/SignalServer.cpp`, `brain/include/brain/SignalServer.hpp` |
| LOW-2 | `pause()`/`resume()` silent no-ops could pass orders through after kill-switch — consider pure virtual or add `is_paused()` guard in engine | LOW | ✅ Done | `exec/include/exec/IExecStrategy.hpp`, `exec/include/exec/ImmediateStrategy.hpp` |
| LOW-3 | `exec.cpp` + `placeholder.cpp` are scaffolding stubs — remove before merge | LOW | ✅ Done | `exec/app/exec.cpp`, `exec/src/exec/placeholder.cpp` |
| LOW-4 | `ArbDetector::flush()` marked `noexcept` but spdlog can throw — wrap body in `try/catch(...)` | LOW | ✅ Done | `brain/include/brain/ArbDetector.hpp` |
| LOW-5 | Premature semicolon in `BrainCmdLine.hpp` options chain breaks readability | LOW | ✅ Done | `brain/include/brain/BrainCmdLine.hpp` |

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
| D5 | Health endpoint: Unix-socket or HTTP endpoint queryable for current feed status without grepping stderr | LOW | ✅ Done | `common/include/utils/HealthServer.hpp`, `brain/app/brain.cpp`, `pop/app/main.cpp`, `brain/include/brain/BrainCmdLine.hpp`, `pop/include/CmdLine.hpp`, `pop/include/md/GenericFeedHandler.hpp`, `brain/include/brain/WsServer.hpp` | `--health-port N` (brain) / `--health_port N` (pop); plain HTTP JSON; 0=disabled; `ok:true` when all synced |
| D6 | Process supervisor config: systemd unit files (or supervisord configs) for auto-restart on crash | LOW | ✅ Done | `deploy/brain.service`, `deploy/pop@.service`, `deploy/supervisord.conf`, `deploy/README.md` | systemd template unit `pop@.service` (one per venue); supervisord alternative; pop now handles SIGTERM gracefully |

---

## Track E — Execution Layer

Full design in `docs/EXECUTION_LAYER_PLAN.md`.

### E — Signal Infrastructure (brain side)

| # | Item | Priority | Status | Files | Notes |
|---|---|---|---|---|---|
| ES1 | `ArbDetector::on_cross_` callback hook: `std::function<void(const ArbCross &)>` member; invoked in `emit_()` after file write | HIGH | ✅ Done | `brain/include/brain/ArbDetector.hpp`, `brain/src/brain/ArbDetector.cpp` | 2-line change; unblocks all downstream work |
| ES2 | `SignalServer`: outbound-only Boost.Beast WS server; `broadcast(text)` pushes ArbCross JSON to all connected exec clients; session outbox + strand; mTLS optional; max 10 subscribers | HIGH | ✅ Done | `brain/include/brain/SignalServer.hpp`, `brain/src/brain/SignalServer.cpp` | Pattern: `WsServer`/`WsSession` |
| ES3 | Wire `SignalServer` in brain: `BrainCmdLine` adds `--signal-port`, `--signal-certfile/keyfile/ca-certfile`; `brain.cpp` instantiates `SignalServer`, sets `arb.on_cross_` to `signal_server.broadcast(...)` | HIGH | ✅ Done | `brain/include/brain/BrainCmdLine.hpp`, `brain/app/brain.cpp` | |

### E — Exec Process (venue side)

| # | Item | Priority | Status | Files | Notes |
|---|---|---|---|---|---|
| EX1 | `exec` subproject scaffold: `exec/CMakeLists.txt`; links `common_core` + `brain_core`; `add_subdirectory(exec)` in root | HIGH | ✅ Done | `exec/CMakeLists.txt`, `CMakeLists.txt` | |
| EX2 | `IOrderClient` pure interface: `Order`/`Fill` structs, `submit_order()`, `cancel_order()` callbacks; `StubOrderClient` logs + fires immediate synthetic fill | HIGH | ✅ Done | `exec/include/exec/IOrderClient.hpp`, `exec/include/exec/StubOrderClient.hpp` | |
| EX3 | `IExecStrategy` pure interface: `on_signal(ArbCross)`, `pause()`, `resume()`; constructed with `IOrderClient&`, `OrderTracker&`, Asio strand; `ImmediateStrategy` (single market order, level-0 qty) as first implementation | HIGH | ✅ Done | `exec/include/exec/IExecStrategy.hpp`, `exec/include/exec/ImmediateStrategy.hpp` | New strategies only need to implement this interface |
| EX4 | `ExecEngine`: E1 position limit, E2 kill switch (SIGUSR1), E3 signal dedup/cooldown, E4 fat-finger notional cap; calls `strategy_->on_signal()` after all guards pass | HIGH | ✅ Done | `exec/include/exec/ExecEngine.hpp`, `exec/src/exec/ExecEngine.cpp` | |
| EX5 | `DeadlineOrderTracker` (E5): Asio `steady_timer` per pending order; on expiry logs TIMEOUT and calls `on_timeout_` callback; `cancel_all()` for clean shutdown | MEDIUM | ✅ Done | `exec/include/exec/OrderTracker.hpp`, `exec/include/exec/DeadlineOrderTracker.hpp`, `exec/src/exec/DeadlineOrderTracker.cpp` | |
| EX6 | `ExecCmdLine` + `exec.cpp` main: connects to brain `SignalServer` via `WsClient`; dispatches `ArbCross` onto exec strand; assembles `StubOrderClient` → `DeadlineOrderTracker` → `ImmediateStrategy` → `ExecEngine` | HIGH | ✅ Done | `exec/include/exec/ExecCmdLine.hpp`, `exec/app/exec.cpp` | |

### E — Extended Strategies (post-MVP)

| # | Item | Priority | Status | Files | Notes |
|---|---|---|---|---|---|
| EE1 | `ThresholdStrategy`: spread-filter decorator wrapping any inner `IExecStrategy`; skips if `spread_bps < --min-spread-bps` | MEDIUM | ⬜ Not Started | `exec/include/exec/ThresholdStrategy.hpp` | Composable wrapper |
| EE2 | `SliceStrategy`: sliced execution with three sizing modes — `equal` (N equal slices), `decay` (geometric decay), `weighted` (custom weight vector); inter-slice delay via Asio timers | LOW | ⬜ Not Started | `exec/include/exec/SliceStrategy.hpp`, `exec/src/exec/Strategies.cpp` | |

---

## Track F — Production Architecture

Larger architectural changes for running at scale or in a high-availability setup.

| # | Item | Priority | Status | Notes |
|---|---|---|---|---|
| F1 | Mutual TLS (mTLS) between PoP and Brain: PoP presents client cert signed by private CA; brain verifies it | HIGH | ✅ Done | `common/include/connection_handler/WsClient.hpp`, `common/src/connection_handler/WsClient.cpp`, `pop/include/postprocess/WsPublishSink.hpp`, `pop/src/postprocess/WsPublishSink.cpp`, `pop/include/abstract/FeedHandler.hpp`, `pop/include/CmdLine.hpp`, `pop/app/main.cpp`, `pop/src/md/GenericFeedHandler.cpp`, `brain/include/brain/BrainCmdLine.hpp`, `brain/app/brain.cpp` | `--brain_ws_certfile/keyfile` on PoP; `--ca-certfile` on brain; opt-in, backward-compatible |
| F2 | Configuration file support (YAML/TOML) with CLI overrides; eliminates long command lines for multi-venue deployments | MEDIUM | ✅ Done | `pop/include/CmdLine.hpp`, `brain/include/brain/BrainCmdLine.hpp` | `--config FILE` (key=value format, boost `parse_config_file`); no new deps; CLI overrides file |
| F3 | Multi-symbol support: one PoP process can track multiple symbols; `FeedHandlerConfig` becomes a symbol list | MEDIUM | ✅ Done | `pop/include/CmdLine.hpp`, `pop/app/main.cpp` | `--symbols BTC/USDT,ETH/USDT`; N handlers share one `io_context`; per-symbol persist path derivation; fully backward-compatible with `--base`/`--quote` |
| F4 | Brain active-passive failover: secondary brain subscribes to same PoPs, promotes on primary failure | LOW | ✅ Done | `--standby` flag + SIGUSR1 promotion; PoP fan-out via `brain2_ws_*`; health shows `standby: true/false` |
| F5 | Latency pipeline: histogram (p50/p95/p99) of PoP-receive → brain-detected latency per venue pair | LOW | ✅ Done | `LatencyHistogram` circular buffer; `lag_ns` in JSONL; `latency_us` in health JSON; logged on flush |
| F6 | Reconnect jitter: ±25% random jitter on all reconnect delays to prevent thundering herd on simultaneous restarts | LOW | ✅ Done | `pop/src/postprocess/WsPublishSink.cpp` | WsPublishSink now uses `mt19937` ±25% jitter matching GenericFeedHandler |

---

## Track G — Concurrency & Scaling

Scaling paths for when symbol count grows or per-message CPU cost increases.
Current single-threaded `io_context` is correct and sufficient for 2–10 symbols (I/O-bound workload).

| # | Item | Priority | Status | Notes |
|---|---|---|---|---|
| G1 | Per-handler thread isolation: one `io_context` + one thread per `GenericFeedHandler`; eliminates single-thread bottleneck for 20+ symbols without strand refactoring | MEDIUM | ✅ Done | `main_ioc` for health/signals; per-handler ioc+thread; work guards; `state_`/`ctr_resyncs_` made atomic for cross-thread health reads |
| G2 | Brain I/O thread pool: call `ioc.run()` from N threads (min 2, max 4, capped at `hardware_concurrency`) to allow concurrent TLS handshakes and reads across all PoP sessions | LOW | ✅ Done | `brain/app/brain.cpp` | Safe: WsServer sessions already use strands; signal handler calls `ioc.stop()` — all threads exit cleanly |
| G3 | Async `FilePersistSink`: move disk writes (gzwrite+flush, ofstream+flush, rotation) off the handler I/O thread onto a bounded internal writer thread (queue cap 10k, drop-oldest) | LOW | ✅ Done | `pop/include/postprocess/FilePersistSink.hpp`, `pop/src/postprocess/FilePersistSink.cpp` | Handler thread enqueues pre-serialised `j.dump()` string and returns immediately; writer thread owns all file handles; destructor signals + joins before process exits |
| G4 | Brain multithreaded architecture: brain runs `ioc.run()` on one thread; arb scan and JSON parsing compete with I/O callbacks. Options: dedicated scan thread fed by lock-free queue; or strand-per-session (WsServer already uses strands) + separate scan thread owning `UnifiedBook`+`ArbDetector` | MEDIUM | ✅ Done | Dedicated scan thread owns `book`+`arb`; I/O thread deserializes + enqueues (mutex+condvar, 50k cap); `HealthSnapshot` replaces direct cross-thread reads; clean join on shutdown |

---

## Track H — Wire Format & Protocol

Performance improvements to the PoP→brain event wire format. Currently all events are
serialised as JSON text frames, which is human-readable but CPU-heavy on both ends.

| # | Item | Priority | Status | Notes |
|---|---|---|---|---|
| H1 | Binary wire format between PoP and Brain: replace `nlohmann::json` serialisation/deserialisation with MessagePack (drop-in, same schema), FlatBuffers (zero-copy, requires schema), or a custom fixed binary layout. Biggest win is on the brain parse side (hot path, single thread). | MEDIUM | ✅ Done | `nlohmann::json::to_msgpack()` / `from_msgpack()` — zero new deps; WS binary frames; backward-compat text-JSON fallback in brain; `send_binary()` added to `WsClient` |
| H2 | Replace `nlohmann/json` in PoP exchange feed parsing (hot path — every tick) with simdjson or RapidJSON. nlohmann builds a full DOM with heap allocation per parse; simdjson parses in-place via SIMD with zero allocation. Profile PoP parse time first to quantify the win. | MEDIUM | ⬜ Not Started | Priority order: PoP feed (hottest) → brain ingestion → signal channel |
| H3 | Replace `nlohmann::from_msgpack()` in brain PoP feed ingestion with a purpose-built msgpack library (mpack or msgpack-c) for zero-copy reads. Currently every incoming PoP frame allocates via nlohmann DOM. | LOW | ⬜ Not Started | Depends on H2 profiling results to confirm this is the next bottleneck |
| H4 | Replace `nlohmann/json` on the brain→exec signal channel with a fixed-size binary struct or MessagePack. Current JSON is ~200 bytes/signal; binary could reduce to ~64 bytes with zero-allocation parsing on exec side. | LOW | ⬜ Not Started | Revisit after ES1–EX6 (exec layer) complete; signal channel does not exist yet |
| H5 | Evaluate moving `WsServer`/`WsSession` and `SignalServer`/`SignalSession` into `common/` if a second process ever needs to accept connections. Currently brain-only so they stay in `brain/`; revisit if exec or a future venue-side server needs an acceptor. | LOW | ⬜ Not Started | No action needed until a second acceptor user exists |

---

## Track I — Python Backtest Environment

Expose the C++ arb detection engine to Python via pybind11 for replaying historical
JSONL data and strategy research. Full design in `docs/backtest.md`.

| # | Item | Priority | Status | Notes |
|---|---|---|---|---|
| I1 | `BacktestEngine` C++ class: thin synchronous wrapper around `UnifiedBook` + `ArbDetector`; no threads, no TLS, no file I/O; `feed_event(json_line) → vector<ArbCross>` | HIGH | ✅ Done | `brain/include/brain/BacktestEngine.hpp`, `brain/src/brain/BacktestEngine.cpp` |
| I2 | `zidabot_replay` subprocess binary: reads JSONL from stdin, writes ArbCross JSON to stdout; Python pipes data through it (pybind11 skipped — spdlog TLS incompatibility with dlopen) | HIGH | ✅ Done | `brain/app/zidabot_replay.cpp`; `cmake --build build --target zidabot_replay` |
| I3 | `python/example_backtest.py`: subprocess-based replay helper + pandas DataFrame output; `docs/HOWTO.md` section 13 | MEDIUM | ✅ Done | |
| I4 | Depth curve access: `--emit-books` flag on zidabot_replay emits `{"type":"book"}` depth curve lines; `BacktestEngine::levels()` + `last_updated_venue()` | LOW | ✅ Done | `brain/include/brain/BacktestEngine.hpp`, `brain/app/zidabot_replay.cpp` |

---

## Batch 11 — ✅ Complete (2026-03-23)

G2 brain I/O thread pool + G3 async `FilePersistSink`.

### Why these two were paired

After G1 (each PoP handler on its own thread) and G4 (brain scan thread), the two remaining
single-threaded bottlenecks were:
- Brain's `ioc.run()` called from one thread — all WS sessions (one per PoP) serialised
- `FilePersistSink::write_line_()` blocking the handler's I/O thread on every `gzflush`/`ofstream::flush`

Both were LOW-priority improvements but required minimal blast radius (3 files total, no
interface changes) so they were batched together.

### G2 — Brain I/O thread pool (`brain/app/brain.cpp`)

**Problem:** Brain ran one thread calling `ioc.run()`. With 2+ PoPs connected, each session's
TLS handshake, async read, and `on_message` callback all serialised on that single thread.
Head-of-line blocking: a slow TLS handshake from one PoP delayed reads from all others.

**Solution:** Replace `ioc.run()` with a pool of `N = min(max(2, hw_concurrency), 4)` threads,
all calling `ioc.run()` on the same `io_context`. This is safe because:
- `WsServer` creates each session's socket with `net::make_strand(ioc_)` — all reads/writes
  for a given session are strand-serialised, so no data race within a session
- `HealthServer`, `watchdog_timer`, and `signal_set` handlers are dispatched through Asio's
  implicit strand — safe for concurrent `ioc.run()` callers
- The `on_message` lambda pushes to `event_queue` under `event_mu` — already mutex-guarded since G4
- Shutdown: signal handler calls `ioc.stop()` — all N threads unblock from `run()` simultaneously

**Change:** 8 lines in `brain/app/brain.cpp` (replaces one `ioc.run()` call).
Logs `[brain] I/O thread pool: N threads` at startup.

### G3 — Async `FilePersistSink` (`pop/include/postprocess/FilePersistSink.hpp`, `pop/src/postprocess/FilePersistSink.cpp`)

**Problem:** `FilePersistSink::write_line_()` was called synchronously from `write_snapshot()`,
`write_incremental()`, and `write_book_state()` — all on the handler's I/O thread (the same thread
that reads from the exchange WebSocket). It performed `j.dump()` (CPU), then `gzwrite()` +
`gzflush(Z_SYNC_FLUSH)` (blocks until kernel flushes compressed bytes to disk) or
`ofstream::flush()`. On a disk stall, IO congestion, or rotation (`std::rename`), the handler
thread was blocked and could not read the next WebSocket message — causing kernel TCP receive
buffer buildup and potential exchange-side disconnection.

`WsPublishSink` (the brain-bound sink) already used a non-blocking outbox queue; `FilePersistSink`
was the only remaining sink that blocked the hot path.

**Solution:** Added an internal writer thread to `FilePersistSink`:
- `enqueue_line_(std::string)` — called on handler thread; locks, pushes to `std::queue<std::string>`,
  notifies, returns immediately. Cap: 10,000 entries; drops oldest on overflow (freshness policy).
- `writer_loop_()` — background thread; waits on condvar, pops, does `gzwrite+gzflush` or
  `ofstream<<+flush`; also owns file rotation (`rotate_()`) and file handle close on exit.
- JSON serialisation (`j.dump()`) stays on the handler thread — it's CPU-only and fast;
  only disk I/O moves to the background.
- `is_open()` now reads `std::atomic<bool> file_open_` — safe to call from the handler thread
  while the writer thread owns the file handles.
- Destructor: `writer_running_=false` + `notify_all` + `join` — queue is fully drained before
  the process exits, so no data is lost on clean shutdown.
- `persist_seq_` is incremented on the calling thread before enqueue — sequence order preserved.

**Change:** 60 lines added across `.hpp` and `.cpp`; 3 call sites (`write_snapshot`,
`write_incremental`, `write_book_state`) updated from `write_line_(j)` to `enqueue_line_(j.dump() + '\n')`.

| # | Item | Status |
|---|---|---|
| G2 | Brain `ioc.run()` replaced with `N`-thread pool; startup log added | ✅ Done |
| G3 | `FilePersistSink` write queue + background writer thread; `is_open()` atomicised | ✅ Done |

---

## Batch 12 — ✅ Complete (2026-03-25)

Python backtest environment (Track I, items I1 + I2 + I3).

pybind11 was attempted but abandoned — GCC's `static thread_local` guard variables in
spdlog produce `R_X86_64_TPOFF32` relocations incompatible with any dlopen'd shared
library regardless of `-fPIC`. Replaced with a subprocess binary approach.

| # | Item | Status |
|---|---|---|
| I1 | `brain/include/brain/BacktestEngine.hpp` + `brain/src/brain/BacktestEngine.cpp` | ✅ Done |
| I2 | `brain/app/zidabot_replay.cpp` subprocess binary (stdin JSONL → stdout JSON crosses) | ✅ Done |
| I3 | `python/example_backtest.py` subprocess-based replay helper + `docs/HOWTO.md` §13 | ✅ Done |

---

## Batch 13B — ✅ Complete (2026-03-31)

Exec-process execution layer: IOrderClient, IExecStrategy, ExecEngine (E1–E4 guards), DeadlineOrderTracker (E5), and exec.cpp main entry point.

| # | Item | Status |
|---|---|---|
| EX2 | `IOrderClient` + `StubOrderClient`: async fill callback contract; stub posts synthetic fill onto exec strand | ✅ Done |
| EX3 | `IExecStrategy` + `ImmediateStrategy`: single market order per leg; `paused_` atomic; `OrderTracker` abstract interface stub | ✅ Done |
| EX4 | `ExecEngine`: E4 (fat-finger) → E1 (position limit) → E2 (kill switch) → E3 (dedup cooldown) → `strategy_->on_signal()` | ✅ Done |
| EX5 | `DeadlineOrderTracker`: `enable_shared_from_this`; `steady_timer` per order; `cancel()` on fill; `cancel_all()` for shutdown | ✅ Done |
| EX6 | `ExecCmdLine` + `exec.cpp`: `WsClient` → exec strand dispatch → `ExecEngine`; `SIGINT/SIGTERM` graceful shutdown | ✅ Done |

60/60 tests passing. `exec` binary builds clean. Full E-track wired end-to-end.

---

## Batch 13A — ✅ Complete (2026-03-29)

Brain-side execution layer: ArbDetector callback hook + SignalServer outbound WS push + brain.cpp wiring.

| # | Item | Status |
|---|---|---|
| ES1 | `ArbDetector::on_cross_` callback: `std::function<void(const ArbCross &)>` member; invoked in `emit_()` after all guards pass | ✅ Done |
| ES2 | `SignalServer` + `SignalSession`: outbound-only TLS WS server; per-session outbox (kMaxOutbox=64, drop-oldest); `broadcast()` safe from any thread via `net::post()` | ✅ Done |
| ES3 | `BrainCmdLine` adds `--signal-port`; `brain.cpp` adds `serialize_cross()`, instantiates `SignalServer`, wires `arb.on_cross_` lambda, adds `signal_server->stop()` to shutdown handler | ✅ Done |

55/55 tests passing. Brain target builds clean.

---

## Batch 10 — ✅ Complete (2026-03-23)

G4 brain dedicated scan thread.

| # | Item | Status |
|---|---|---|
| G4 | Event queue (`std::deque<nlohmann::json>`, mutex+condvar, 50k cap) between I/O thread and scan thread | ✅ Done |
| G4 | Scan thread owns `UnifiedBook` + `ArbDetector`; calls `on_event()` + `scan()` without blocking I/O | ✅ Done |
| G4 | `HealthSnapshot` struct updated by scan thread (under `health_mu`); health + watchdog callbacks read snapshot — no direct cross-thread access to `book`/`arb` | ✅ Done |
| G4 | Shutdown: `scan_running=false` + `notify_all` → scan thread exits loop → `join` before `arb.flush()` | ✅ Done |

---

## Batch 9 — ✅ Complete (2026-03-22)

G1 per-handler thread isolation + H1 MessagePack wire format.

| # | Item | Status |
|---|---|---|
| G1 | `state_` → `std::atomic<FeedSyncState>`; `ctr_resyncs_` → `std::atomic<uint64_t>` — safe cross-thread health reads | ✅ Done |
| G1 | `main_ioc` (health + signals) + per-handler `io_context` + `std::thread`; work guards; signal posts stop then releases guard | ✅ Done |
| H1 | `send_binary()` added to `WsClient`; outbox tagged with `{data, bool binary}`; `ws_.text()` set per frame in `do_write_()` | ✅ Done |
| H1 | `WsPublishSink::send_json_()` → `nlohmann::json::to_msgpack()` + `send_binary()` | ✅ Done |
| H1 | `WsServer::MessageHandler` → `void(string_view, bool is_binary)`; `WsSession` handles both text+binary frames | ✅ Done |
| H1 | `brain::on_message` branches on `is_binary`: `from_msgpack()` for binary, `parse()` for text (backward-compat window) | ✅ Done |

---

## Batch 8 — ✅ Complete (2026-03-22)

F4 active-passive brain failover + F5 latency histogram.

| # | Item | Status |
|---|---|---|
| F4 | `--standby` flag on brain: receives data, builds state, emits nothing until `kill -USR1 <pid>` | ✅ Done |
| F4 | PoP `brain2_ws_*` config block: fan-out to primary + standby brain simultaneously | ✅ Done |
| F4 | SIGUSR1 handler in brain promotes standby → active atomically (no restart needed) | ✅ Done |
| F5 | `LatencyHistogram`: 10 000-sample circular buffer; p50/p95/p99 via sort on flush/query | ✅ Done |
| F5 | `lag_ns = ts_detected_ns − max(sell_ts_book_ns, buy_ts_book_ns)` recorded per cross | ✅ Done |
| F5 | `latency_us` added to brain health JSON; `lag_ns` added to arb JSONL; logged on `flush()` | ✅ Done |

---

## Batch 7 — ✅ Complete (2026-03-21)

D5 health endpoint + D6 process supervisor configs. Pop SIGTERM handling added as prerequisite.

| # | Item | Status |
|---|---|---|
| — | Pop: add SIGTERM/SIGINT handler (`boost::asio::signal_set`) — prerequisite for systemd `stop` | ✅ Done |
| D5 | `HealthServer` header-only HTTP health class; `--health-port` / `--health_port` on both processes; JSON with sync state, uptime, per-venue/handler details | ✅ Done |
| D6 | `deploy/brain.service`, `deploy/pop@.service` (systemd template), `deploy/supervisord.conf`, `deploy/README.md` | ✅ Done |

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
