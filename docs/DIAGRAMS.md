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

    subgraph Brain ["Brain Process  (single instance)"]
        WS[WsServer / WsSession]
        UB[UnifiedBook]
        AD[ArbDetector]
        SS[SignalServer / SignalSession]
        JSONL[(arb.jsonl)]
    end

    subgraph Exec ["Exec Processes  (one per venue)"]
        WC[WsClient]
        EE[ExecEngine\nE1–E4 guards]
        IS[ImmediateStrategy]
        OT[DeadlineOrderTracker]
        OC[StubOrderClient]
    end

    EX1 -->|WS frames| F1
    EX2 -->|WS frames| F2
    EX3 -->|WS frames| F3
    EX4 -->|WS frames| F4

    F1 -->|book_state JSON\nTLS WS| WS
    F2 -->|book_state JSON\nTLS WS| WS
    F3 -->|book_state JSON\nTLS WS| WS
    F4 -->|book_state JSON\nTLS WS| WS

    WS --> UB
    UB -->|venue updated| AD
    AD -->|ArbCross| SS
    AD -->|ArbCross| JSONL

    SS -->|ArbCross JSON\nTLS WS| WC
    WC -->|dispatch → exec strand| EE
    EE -->|on_signal| IS
    IS --> OT
    IS --> OC
```

---

## 2. ArbCross Data Flow

How a single arbitrage signal travels from detection in the brain to order dispatch in exec.

```mermaid
flowchart LR
    A([ArbDetector\nscan]) -->|emit_| B[on_cross_ callback]
    B -->|broadcast\nJSON text| C[SignalServer]
    C -->|TLS WS frame| D[WsClient\non_raw_message]
    D -->|asio::dispatch\nexec strand| E[parse_cross]
    E -->|ArbCross| F[ExecEngine\non_signal]

    F --> G{E4 fat-finger\nprice > cap?}
    G -->|reject| Z([drop])
    G -->|pass| H{E1 position\nlimit breached?}
    H -->|reject| Z
    H -->|pass| I{E2 kill\nswitch paused?}
    I -->|reject| Z
    I -->|pass| J{E3 cooldown\nnot elapsed?}
    J -->|reject| Z
    J -->|pass| K[ImmediateStrategy\non_signal]

    K -->|register_pending| L[DeadlineOrderTracker\narm steady_timer]
    K -->|submit_order| M[StubOrderClient]
    M -->|fill callback\nexec strand| N[on_fill]
    N -->|cancel timer| L
    N -->|open_notional_ +=| F
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

    IOrderClient <|-- StubOrderClient : implements
    OrderTracker <|-- DeadlineOrderTracker : implements
    IExecStrategy <|-- ImmediateStrategy : implements
    ImmediateStrategy --> IOrderClient : uses
    ImmediateStrategy --> OrderTracker : uses
    ExecEngine *-- IExecStrategy : owns
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
    participant OC as StubOrderClient

    AD->>SS: broadcast(ArbCross JSON)
    SS-->>WC: TLS WS text frame

    WC->>WC: on_raw_message (WsClient strand)
    WC->>EE: asio::dispatch → exec strand<br/>on_signal(ArbCross)

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
```
