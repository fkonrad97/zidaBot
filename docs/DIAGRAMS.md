# Architecture Diagrams

Rendered natively on GitHub and in VS Code (Markdown Preview Mermaid Support extension).

---

## 1. Component Architecture

Full stack from exchange WebSocket feeds through PoP/feed ingestion, brain arb detection, and exec order dispatch.

```mermaid
graph TB
    subgraph Exchanges
        EX1[Binance WS/REST]
        EX2[OKX WS/REST]
        EX3[Bybit WS/REST]
        EX4[KuCoin WS/REST]
    end

    subgraph Feed Processes ["Feed Processes  (one per venue)"]
        F1[feed — binance]
        F2[feed — okx]
        F3[feed — bybit]
        F4[feed — kucoin]
    end

    subgraph Brain ["Brain Process"]
        WS[WsServer / WsSession]
        UB[UnifiedBook]
        AD[ArbDetector]
        SS[SignalServer / SignalSession]
        JSONL[(arb.jsonl)]
    end

    subgraph Exec ["Exec Processes (per venue)"]
        WC[WsClient]
        EE[ExecEngine]
        IS[ImmediateStrategy]
        OT[DeadlineOrderTracker]
        DC[DryRunOrderClient]
        LG[Live rollout guards]
        OC[Venue order client]
    end

    EX1 -->|WS frames| F1
    EX2 -->|WS frames| F2
    EX3 -->|WS frames| F3
    EX4 -->|WS frames| F4

    F1 -->|book_state JSON - TLS WS| WS
    F2 -->|book_state JSON - TLS WS| WS
    F3 -->|book_state JSON - TLS WS| WS
    F4 -->|book_state JSON - TLS WS| WS

    WS --> UB
    UB -->|venue updated| AD
    AD -->|ArbCross| SS
    AD -->|ArbCross| JSONL

    SS -->|ArbCross JSON - TLS WS| WC
    WC -->|dispatch → exec strand| EE
    LG -->|startup validation| EE
    EE -->|on_signal| IS
    IS --> OT
    IS --> DC
    IS --> OC
```

---

## 2. ArbCross Data Flow

How a single arbitrage signal travels from detection in the brain to order dispatch in exec.

```mermaid
flowchart LR
    A([ArbDetector - scan]) -->|emit_| B[on_cross_ callback]
    B -->|broadcast - JSON text| C[SignalServer]
    C -->|TLS WS frame| D[WsClient - on_raw_message]
    D -->|asio::dispatch - exec strand| E[parse_cross]
    E -->|ArbCross| F[ExecEngine - on_signal]

    F --> G{E4 fat-finger - price > cap?}
    G -->|reject| Z([drop])
    G -->|pass| H{E1 position - limit breached?}
    H -->|reject| Z
    H -->|pass| I{E2 kill - switch paused?}
    I -->|reject| Z
    I -->|pass| J{E3 cooldown - not elapsed?}
    J -->|reject| Z
    J -->|pass| K[ImmediateStrategy - on_signal]

    K -->|register_pending| L[DeadlineOrderTracker - arm steady_timer]
    K -->|submit_order| M[Order client]
    M -->|fill callback - exec strand| N[on_fill]
    N -->|cancel timer| L
    N -->|open_notional_ +=| F

    Note over M: In dry-run mode, Order client = DryRunOrderClient
    Note over M: In live mode, startup guards must pass first
```

---

## 3. Exec Layer Class Diagram

Interfaces, concrete implementations, and ownership relationships in the exec/ module.

```mermaid
classDiagram
    class IOrderClient {
        <<interface>>
        +submit_order(Order, callback) void
        +cancel_order(string, callback) void
    }

    class StubOrderClient {
        -strand_ strand
        +submit_order(Order, callback) void
        +cancel_order(string, callback) void
    }

    class DryRunOrderClient {
        -strand_ strand
        +submit_order(Order, callback) void
        +cancel_order(string, callback) void
    }

    class OrderTracker {
        <<interface>>
        +register_pending(Order) void
        +on_fill(Fill) void
    }

    class DeadlineOrderTracker {
        -strand_ strand
        -timeout_ milliseconds
        -on_timeout_ TimeoutCb
        -pending_ unordered_map
        +create() shared_ptr$
        +register_pending(Order) void
        +on_fill(Fill) void
        +cancel_all() void
        +pending_count() size_t
    }

    class IExecStrategy {
        <<interface>>
        +on_signal(ArbCross) void
        +pause() void
        +resume() void
        +is_paused() bool
    }

    class ImmediateStrategy {
        -my_venue_ string
        -client_ IOrderClient&
        -tracker_ OrderTracker&
        -on_fill_ function
        -strand_ strand
        -target_qty_ double
        -paused_ atomic~bool~
        -order_seq_ atomic~uint64~
        +on_signal(ArbCross) void
        +pause() void
        +resume() void
        +is_paused() bool
    }

    class ExecEngine {
        -my_venue_ string
        -paused_ atomic~bool~
        -open_notional_ double
        -last_signal_ns_ unordered_map
        -position_limit_ double
        -max_order_notional_ double
        -cooldown_ns_ int64
        -strategy_ unique_ptr
        +on_signal(ArbCross) void
        +on_fill(Fill) void
        +pause() void
        +resume() void
    }

    class ExecOptions {
        +dry_run bool
        +arm_live bool
        +enable_live_venue string
        +live_order_notional_cap double
    }

    IOrderClient <|-- StubOrderClient : implements
    IOrderClient <|-- DryRunOrderClient : implements
    OrderTracker <|-- DeadlineOrderTracker : implements
    IExecStrategy <|-- ImmediateStrategy : implements
    ImmediateStrategy --> IOrderClient : uses
    ImmediateStrategy --> OrderTracker : uses
    ExecEngine *-- IExecStrategy : owns
    ExecOptions --> ExecEngine : configures startup mode
```

---

## 4. Signal Lifecycle Sequence

One arb cross from brain detection to fill confirmation, including the E1–E4 guard chain and the E5 deadline timer.

```mermaid
sequenceDiagram
    participant AD as ArbDetector
    participant SS as SignalServer
    participant WC as WsClient
    participant EE as ExecEngine
    participant IS as ImmediateStrategy
    participant OT as DeadlineOrderTracker
    participant OC as OrderClient

    AD->>SS: broadcast(ArbCross JSON)
    SS-->>WC: TLS WS text frame

    WC->>WC: on_raw_message (WsClient strand)
    WC->>EE: asio::dispatch → exec strand<br/>on_signal(ArbCross)

    Note over EE: Startup mode
    Note over EE: default = dry-run
    Note over EE: live requires --live-mode + --arm-live
    Note over EE: live also requires exact venue gate
    Note over EE: and tiny-notional clamp

    Note over EE: E4: ref_price ≤ max_order_notional?
    Note over EE: E1: open_notional + new ≤ position_limit?
    Note over EE: E2: paused_ == false?
    Note over EE: E3: ts - last_signal ≥ cooldown_ns?

    EE->>IS: on_signal(ArbCross)
    IS->>OT: register_pending(Order)
    OT->>OT: arm steady_timer (confirmation_timeout_ms)
    IS->>OC: submit_order(Order, fill_cb)

    OC->>OC: post fill_cb onto exec strand

    alt fill arrives before deadline
        OC-->>IS: fill_cb({}, Fill)
        IS->>OT: on_fill(Fill)
        OT->>OT: timer->cancel() → operation_aborted
        IS->>EE: engine.on_fill(Fill)
        EE->>EE: open_notional_ += qty × price
    else deadline expires (no fill)
        OT->>OT: steady_timer fires
        OT-->>EE: on_timeout_(client_order_id)
        Note over EE: log WARN — future: pause engine
    end

    Note over OC: DryRunOrderClient synthesizes fills locally
    Note over OC: Live order clients are future venue adapters
```

---

## 5. Feed Layer Class Diagram

Classes in `feed/` (per-venue market data ingestion) and the shared `common/` orderbook library.

```mermaid
classDiagram
    class IVenueFeedHandler {
        <<interface>>
        +init(FeedHandlerConfig) FeedOpResult
        +start() FeedOpResult
        +stop() FeedOpResult
    }

    class FeedHandlerConfig {
        +venue_name VenueId
        +symbol string
        +base_ccy string
        +quote_ccy string
        +ws_host / ws_port / ws_path string
        +rest_host / rest_port / rest_path string
        +brain_ws_host / brain_ws_port string
        +brain_ws_insecure bool
        +persist_path string
        +persist_book_every_updates size_t
        +persist_book_top size_t
        +max_msg_rate_per_sec int
    }

    class GenericFeedHandler {
        -ioc_ io_context
        -adapter_ VenueAdapter variant
        -controller_ OrderBookController
        -ws_client_ WsClient
        -file_sink_ FilePersistSink
        -ws_sink_ WsPublishSink
        -sync_state_ SyncState
        +init(FeedHandlerConfig) FeedOpResult
        +start() FeedOpResult
        +stop() FeedOpResult
    }

    class VenueAdapter {
        <<variant>>
        BinanceAdapter
        OkxAdapter
        BybitAdapter
        BitgetAdapter
        KucoinAdapter
        +parse_ws_frame(msg) variant
        +snapshot_url() string
        +subscribe_msg() string
    }

    class OrderBookController {
        -book_ OrderBook
        -state_ SyncState
        -checksum_fn_ ChecksumFn
        +apply_snapshot(GenericSnapshotFormat) Action
        +apply_incremental(GenericIncrementalFormat) Action
        +apply_ws_authoritative(GenericSnapshotFormat) Action
        +setAllowSequenceGap(bool)
        +configureChecksum(fn, topN)
    }

    class OrderBook {
        -bids_ vector~Level~
        -asks_ vector~Level~
        -depth_ size_t
        +apply(levels, side)
        +best_bid() Level
        +best_ask() Level
        +top(n, side) vector~Level~
        +validate() bool
    }

    class WsClient {
        -strand_ strand
        -ws_ websocket_stream
        -outbox_ deque
        +connect(host, port, path)
        +send_text(string)
        +send_binary(string)
        +set_on_raw_message(handler)
        +set_tls_verify_peer(bool)
        +set_client_cert(cert, key)
        +close()
    }

    class FilePersistSink {
        -path_ string
        -update_count_ size_t
        +on_snapshot(json)
        +on_incremental(json)
        +on_book_state(OrderBook, seq)
    }

    class WsPublishSink {
        -ws_client_ WsClient
        -update_count_ size_t
        +on_snapshot(json)
        +on_incremental(json)
        +on_book_state(OrderBook, seq)
    }

    IVenueFeedHandler <|-- GenericFeedHandler : implements
    GenericFeedHandler *-- VenueAdapter : owns (variant)
    GenericFeedHandler *-- OrderBookController : owns
    GenericFeedHandler *-- WsClient : owns
    GenericFeedHandler *-- FilePersistSink : owns
    GenericFeedHandler *-- WsPublishSink : owns
    OrderBookController *-- OrderBook : owns
    WsPublishSink --> WsClient : uses
```

---

## 6. Brain Layer Class Diagram

Classes in `brain/` — inbound feed server, book aggregation, arb detection, and outbound signal push.

```mermaid
classDiagram
    class WsServer {
        -ioc_ io_context
        -ssl_ctx_ ssl_context
        -acceptor_ tcp_acceptor
        -sessions_ vector~weak_ptr~
        -on_message_ MessageHandler
        +start()
        +stop()
        +session_count() size_t
    }

    class WsSession {
        -ws_ WsStream
        -buffer_ flat_buffer
        -on_message_ MessageHandler
        -remote_addr_ string
        +run()
        +close()
    }

    class UnifiedBook {
        -books_ vector~VenueBook~
        -depth_ size_t
        +on_event(json) string
        +venues() vector~VenueBook~
        +synced_count() size_t
    }

    class VenueBook {
        +venue_name string
        +symbol string
        +controller unique_ptr~OrderBookController~
        +ts_book_ns int64
        +feed_healthy bool
        +synced() bool
        +book() OrderBook
    }

    class ArbDetector {
        -min_spread_bps_ double
        -max_spread_bps_ double
        -rate_limit_ns_ int64
        -max_age_ns_ int64
        -on_cross_ function
        +scan(venues vector~VenueBook~)
        +set_active(bool)
        +is_active() bool
        +last_cross_ns() int64
    }

    class ArbCross {
        +sell_venue string
        +buy_venue string
        +sell_bid_tick int64
        +buy_ask_tick int64
        +spread_bps double
        +ts_detected_ns int64
        +sell_ts_book_ns int64
        +buy_ts_book_ns int64
    }

    class SignalServer {
        -ioc_ io_context
        -ssl_ctx_ ssl_context
        -acceptor_ tcp_acceptor
        -strand_ strand
        -sessions_ vector~weak_ptr~
        -live_count_ atomic_size_t
        +start()
        +stop()
        +broadcast(string)
        +session_count() size_t
    }

    class SignalSession {
        -ws_ WsStream
        -outbox_ deque~string~
        -writing_ bool
        -kMaxOutbox = 64
        +run()
        +close()
        +send(string)
    }

    WsServer "1" *-- "0..*" WsSession : accepts
    UnifiedBook "1" *-- "0..*" VenueBook : owns
    VenueBook *-- OrderBookController : owns
    ArbDetector --> VenueBook : reads books
    ArbDetector ..> ArbCross : emits
    ArbDetector --> SignalServer : on_cross_ callback
    SignalServer "1" *-- "0..*" SignalSession : accepts
```
