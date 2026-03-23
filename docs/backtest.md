# ZidaBot — Python Backtest Environment

Design plan for exposing the C++ arb detection engine to Python via pybind11,
enabling replay of historical JSONL data and strategy research in a familiar
Python/pandas environment.

---

## Goals

1. Replay the normalized event files produced by `--persist_path` through the
   **exact same C++ logic** that runs in production (same order book, same arb
   detector, same guards).
2. Return results as plain Python objects — no JSON round-trips.
3. Optionally expose live order book state per venue at each event (for feature
   engineering: spread time-series, depth curves, etc.).
4. Zero duplication: the C++ code is not rewritten — bindings just call it.

---

## Data flow

```
JSONL replay files        pybind11 module         Python
(--persist_path output)   ───────────────────     ──────────────────────────
                                                  engine = BacktestEngine(...)
binance_BTC_USDT.jsonl ─► engine.feed_event() ──► [ArbCross, ...]
okx_BTC_USDT.jsonl     ─►                     ──► bbo per venue (optional)
...                                                pandas DataFrame / charts
```

The JSONL files already contain fully normalized events in the schema that
`brain` consumes:

| Event type | When written |
|---|---|
| `snapshot` | On REST or WS snapshot |
| `incremental` | Each delta update |
| `book_state` | Every N updates (`--persist_book_every_updates`) |
| `status` | Feed health transitions |

These map directly to `UnifiedBook::on_event()` → `ArbDetector::scan()`,
the exact same path as production.

---

## Python API design

### `BacktestEngine`

```python
from zidabot import BacktestEngine, ArbCross

engine = BacktestEngine(
    depth=50,                     # order book depth per venue
    min_spread_bps=0.0,           # same semantics as --min-spread-bps
    max_spread_bps=0.0,           # 0 = no cap
    rate_limit_ms=0,              # 0 = unlimited (useful for backtesting)
    max_age_ms=5000,              # staleness guard in ms
    max_price_deviation_pct=0.0,  # 0 = off
)

# Feed one raw JSON line from a JSONL file.
# Returns all crosses detected by this event (empty list = no cross).
crosses: list[ArbCross] = engine.feed_event(line)

# Optional: read live BBO per venue after each event.
bbo = engine.bbo("binance")  # {"bid_tick": int, "ask_tick": int, "ts_ns": int}

# Current number of synced venues.
n: int = engine.synced_count()

# Reset all book state (start a new replay window).
engine.reset()
```

### `ArbCross` (Python dataclass)

Maps 1:1 to `brain::ArbCross` in `brain/include/brain/ArbDetector.hpp`:

```python
@dataclass
class ArbCross:
    sell_venue:      str    # venue where we sell (their bid > other's ask)
    buy_venue:       str    # venue where we buy
    sell_bid_tick:   int    # integer price ticks — no float rounding
    buy_ask_tick:    int
    spread_bps:      float
    ts_detected_ns:  int
    sell_ts_book_ns: int
    buy_ts_book_ns:  int
    lag_ns:          int    # ts_detected_ns - max(sell_ts_book_ns, buy_ts_book_ns)
```

### Convenience replay helper (pure Python, ships with the module)

```python
import gzip
from zidabot import BacktestEngine

def replay(files: list[str], **engine_kwargs) -> list[ArbCross]:
    """Replay a list of JSONL (or .jsonl.gz) files, return all crosses."""
    engine = BacktestEngine(**engine_kwargs)
    results = []
    for path in sorted(files):          # sort by name to get chronological order
        opener = gzip.open if path.endswith(".gz") else open
        with opener(path, "rt") as f:
            for line in f:
                results.extend(engine.feed_event(line.strip()))
    return results

# Example usage
crosses = replay(
    ["data/binance_BTC_USDT.jsonl.gz", "data/okx_BTC_USDT.jsonl.gz"],
    min_spread_bps=0.5,
    rate_limit_ms=0,
)

import pandas as pd
df = pd.DataFrame([vars(c) for c in crosses])
df["spread_bps"].hist(bins=50)
df.groupby(["sell_venue", "buy_venue"])["spread_bps"].describe()
```

---

## Implementation plan

### Step 1 — Add pybind11 dependency

pybind11 is header-only. Integrate via CMake FetchContent (no system package needed):

```cmake
# root CMakeLists.txt — add after existing FetchContent blocks
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.1
)
FetchContent_MakeAvailable(pybind11)
```

No conflict with Boost/nlohmann/spdlog.

### Step 2 — New `BacktestEngine` C++ class

**`brain/include/brain/BacktestEngine.hpp`** (new)

Thin synchronous wrapper — no `io_context`, no threads, no TLS, no file output:

```cpp
namespace brain {

class BacktestEngine {
public:
    BacktestEngine(std::size_t depth,
                   double min_spread_bps,
                   double max_spread_bps,
                   std::int64_t rate_limit_ns,
                   std::int64_t max_age_ns,
                   double max_price_deviation_pct);

    // Feed one JSON string (one JSONL line). Returns detected crosses.
    std::vector<ArbCross> feed_event(std::string_view json_line);

    // Number of currently synced venues.
    std::size_t synced_count() const noexcept;

    // BBO for a named venue (all zeros if not synced).
    struct BBO { std::int64_t bid_tick{0}; std::int64_t ask_tick{0}; std::int64_t ts_ns{0}; };
    BBO bbo(const std::string &venue) const noexcept;

    // Reset all state (start new replay window).
    void reset();

private:
    std::size_t depth_;
    UnifiedBook book_;
    ArbDetector arb_;   // output_path="" disables file writing
};

} // namespace brain
```

`feed_event()` body — three lines:

```cpp
std::vector<ArbCross> BacktestEngine::feed_event(std::string_view json_line) {
    try {
        const auto j = nlohmann::json::parse(json_line);
        const std::string updated = book_.on_event(j);
        if (!updated.empty() && book_.synced_count() >= 2)
            return arb_.scan(book_.venues());   // already returns vector<ArbCross>
    } catch (...) {}
    return {};
}
```

No changes to existing classes. `ArbDetector::scan()` already returns
`std::vector<ArbCross>` — the production brain just ignores the return value.

### Step 3 — pybind11 binding module

**`python/zidabot.cpp`** (new):

```cpp
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>         // std::vector<ArbCross> → list[ArbCross]
#include "brain/BacktestEngine.hpp"

namespace py = pybind11;
using namespace brain;

PYBIND11_MODULE(zidabot, m) {
    m.doc() = "ZidaBot backtesting engine";

    py::class_<ArbCross>(m, "ArbCross")
        .def_readonly("sell_venue",      &ArbCross::sell_venue)
        .def_readonly("buy_venue",       &ArbCross::buy_venue)
        .def_readonly("sell_bid_tick",   &ArbCross::sell_bid_tick)
        .def_readonly("buy_ask_tick",    &ArbCross::buy_ask_tick)
        .def_readonly("spread_bps",      &ArbCross::spread_bps)
        .def_readonly("ts_detected_ns",  &ArbCross::ts_detected_ns)
        .def_readonly("sell_ts_book_ns", &ArbCross::sell_ts_book_ns)
        .def_readonly("buy_ts_book_ns",  &ArbCross::buy_ts_book_ns)
        .def_property_readonly("lag_ns", [](const ArbCross &c) {
            return c.ts_detected_ns - std::max(c.sell_ts_book_ns, c.buy_ts_book_ns);
        })
        .def("__repr__", [](const ArbCross &c) {
            return "ArbCross(sell=" + c.sell_venue + " buy=" + c.buy_venue
                   + " spread_bps=" + std::to_string(c.spread_bps) + ")";
        });

    py::class_<BacktestEngine::BBO>(m, "BBO")
        .def_readonly("bid_tick", &BacktestEngine::BBO::bid_tick)
        .def_readonly("ask_tick", &BacktestEngine::BBO::ask_tick)
        .def_readonly("ts_ns",    &BacktestEngine::BBO::ts_ns);

    py::class_<BacktestEngine>(m, "BacktestEngine")
        .def(py::init<std::size_t, double, double,
                      std::int64_t, std::int64_t, double>(),
             py::arg("depth")                   = 50,
             py::arg("min_spread_bps")          = 0.0,
             py::arg("max_spread_bps")          = 0.0,
             py::arg("rate_limit_ns")           = 0,
             py::arg("max_age_ns")              = 5'000'000'000LL,
             py::arg("max_price_deviation_pct") = 0.0)
        .def("feed_event",   &BacktestEngine::feed_event,  py::arg("json_line"))
        .def("synced_count", &BacktestEngine::synced_count)
        .def("bbo",          &BacktestEngine::bbo,         py::arg("venue"))
        .def("reset",        &BacktestEngine::reset);
}
```

### Step 4 — CMake target

**`python/CMakeLists.txt`** (new):

```cmake
pybind11_add_module(zidabot zidabot.cpp)
target_link_libraries(zidabot PRIVATE brain_core common_core)
target_include_directories(zidabot PRIVATE
    ${CMAKE_SOURCE_DIR}/brain/include
    ${CMAKE_SOURCE_DIR}/common/include)
```

Add to root `CMakeLists.txt`:
```cmake
add_subdirectory(python)
```

Build and use:
```bash
cmake --build build -j4 --target zidabot
# produces: build/python/zidabot.cpython-3XX-*.so

export PYTHONPATH=$PWD/build/python
python3 -c "import zidabot; print('ok')"
```

---

## Files to create

| File | Lines | Description |
|---|---|---|
| `brain/include/brain/BacktestEngine.hpp` | ~40 | Thin wrapper: `UnifiedBook` + `ArbDetector`, no async |
| `brain/src/brain/BacktestEngine.cpp` | ~60 | `feed_event`, `bbo`, `reset` implementations |
| `python/zidabot.cpp` | ~60 | pybind11 binding module |
| `python/CMakeLists.txt` | ~10 | `pybind11_add_module` |
| `python/example_backtest.py` | ~40 | Working replay example with pandas |

**Files not changed:** all existing C++ sources. `BacktestEngine` is additive only.

---

## Key design decisions

| Decision | Rationale |
|---|---|
| Wrap `BacktestEngine`, not `UnifiedBook`/`ArbDetector` directly | Simpler Python API; no need to expose `VenueBook`, `OrderBookController`, etc. |
| `feed_event(str)` takes a raw JSON line | Matches JSONL format exactly; caller owns I/O and line iteration |
| `rate_limit_ms=0` as Python default | In backtesting you usually want every cross, not rate-limited ones |
| No output file in `BacktestEngine` | Python caller collects crosses directly from the return value |
| `lag_ns` as a computed Python property | Derived from existing fields — no C++ struct change needed |
| `pybind11/stl.h` | Gives automatic `std::vector<ArbCross>` → `list[ArbCross]` with no manual loop |
| FetchContent for pybind11 | No system package dependency; version-pinned; same pattern as existing deps |

---

## Out of scope (future extensions)

- **Depth curves**: expose full bid/ask level arrays per venue per event — useful for
  feature engineering but requires binding `OrderBook::bid_ptr(i)` / `ask_ptr(i)`
- **Multi-symbol replay**: already supported by `UnifiedBook` — just feed events from
  multiple JSONL files interleaved by `ts_recv_ns`
- **Numpy price arrays**: accumulate `(ts_ns, bid_tick, ask_tick)` per venue into
  pre-allocated numpy arrays for faster pandas ingestion
- **`setuptools` / `pyproject.toml`**: package as a proper pip-installable wheel
  once the API stabilises
