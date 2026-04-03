# FIX.md — Issue Backlog

Issues found during code review and security review of 2026-04-01 changes.
Agents run: `code-reviewer`, `security-reviewer`, `test-runner`, `project-manager`.
All 92 tests passing. Items below are backlog fixes, not build blockers.

---

## CRITICAL / URGENT

### FIX-1 — E1 position limit silently disabled end-to-end (live bug)

**Severity:** CRITICAL (functional — safety guard is a no-op in production)

**What:** `ImmediateStrategy`'s fill callback calls `tracker_.on_fill()` (E5 timeout) but never calls `engine_.on_fill()` (E1 position accounting). `ExecEngine::open_notional_` is therefore never incremented through the live pipeline. The E1 position limit guard (`--position-limit`) is fully bypassed; the engine will never block on position size regardless of config.

**Where:**
- `exec/src/exec/Strategies.cpp` — fill callback in `ImmediateStrategy::on_signal()`
- `tests/exec/test_exec_pipeline.cpp:172` — `E1WiringGap_EngineOnFillDirectly` documents the gap

**Fix:** After `client_->submit_order(...)` registers the callback, wire the callback to also call `engine_.on_fill(qty, price_tick)`. Update `E1WiringGap_EngineOnFillDirectly` to verify `open_notional_` is incremented.

---

### FIX-2 — Negative fee silently accepted, inflates net spread

**Severity:** HIGH (security — masks unprofitable trades as profitable)

**What:** `parse_venue_fees()` uses `catch(...)` but `std::stod("-5")` succeeds — it returns `-5.0`. A negative fee inflates `net_spread_bps = spread - (-5) - other_fee`, causing the min-spread filter to pass crosses that are actually unprofitable.

**Where:** `brain/include/brain/BrainCmdLine.hpp:24-26`

**Fix:**
```cpp
const double fee = std::stod(token.substr(colon + 1));
if (fee < 0.0) {
    spdlog::warn("[brain] venue-fees: negative fee ignored for '{}'", token);
    continue;
}
result[token.substr(0, colon)] = fee;
```

---

### FIX-3 — Malformed fee tokens silently dropped, no warning logged

**Severity:** HIGH (operational — typo `okx` instead of `okx:8` silently zeroes OKX fee)

**What:** When `parse_venue_fees()` skips a malformed token (missing colon, empty value, bad number), it does so silently. The operator gets no feedback that a fee entry was ignored; the venue defaults to 0 bps, understating cost and over-signalling.

**Where:** `brain/include/brain/BrainCmdLine.hpp:20-26`

**Fix:** Replace the bare `continue` in malformed-token branches with a `spdlog::warn`:
```cpp
if (colon == std::string::npos || colon == 0 || colon + 1 == token.size()) {
    spdlog::warn("[brain] venue-fees: malformed token '{}' skipped", token);
    continue;
}
```
Same for the `catch(...)` block.

---

## HIGH

### FIX-4 — `net_spread_bps` uninitialized in `make_cross()` test helper

**Severity:** HIGH (test robustness — undefined value; will break if any guard reads it)

**What:** `make_cross()` in `tests/exec/test_exec_engine.cpp` constructs an `ArbCross` but never sets `net_spread_bps`. The field holds garbage. Tests pass today because `ExecEngine` guards don't read it — but this is brittle.

**Where:** `tests/exec/test_exec_engine.cpp:24-38`

**Fix:** Add `c.net_spread_bps = c.spread_bps;` (or `= 0.0`) in `make_cross()`.

---

### FIX-5 — JSONL write queue overflow drops silently

**Severity:** HIGH (observability — data loss in JSONL output with no operator alert)

**What:** When `write_queue_` reaches `kWriteQueueCap` (4096), `emit_()` pops the oldest entry silently: `write_queue_.pop_front()`. Under burst arb conditions the JSONL file gets gaps with no warning.

**Where:** `brain/src/brain/ArbDetector.cpp:144-147`

**Fix:** Log once on first overflow:
```cpp
static std::atomic<uint64_t> drop_count{0};
if (write_queue_.size() >= kWriteQueueCap) {
    write_queue_.pop_front();
    if (++drop_count == 1)
        spdlog::warn("[ArbDetector] JSONL write queue overflowed — drops occurring");
}
```

---

### FIX-6 — Duplicate JSONL serialization in `emit_()` and `serialize_cross()`

**Severity:** HIGH (maintainability — two code paths to update on every new ArbCross field)

**What:** `ArbDetector::emit_()` serializes `ArbCross` to JSONL. `brain.cpp::serialize_cross()` does the same for the exec signal. They are almost identical but `emit_()` includes `lag_ns` while `serialize_cross()` does not. Every new field must be added in two places.

**Where:**
- `brain/src/brain/ArbDetector.cpp:129-142`
- `brain/app/brain.cpp:50-65`

**Fix:** Extract a single `nlohmann::json arb_cross_to_json(const ArbCross &)` free function (e.g., in `ArbDetector.hpp`). Both call sites use it; `emit_()` adds `lag_ns` afterward.

---

### FIX-7 — Fee lookup on hot path undocumented, no performance rationale

**Severity:** HIGH (performance — `unordered_map::find()` per venue pair per tick inside scan inner loop)

**What:** `fee_for_()` does an `unordered_map` lookup for every directed venue pair on every scan. For 5 venues this is 20 lookups/scan; for 16 it is 240. No comment explains why this is acceptable or what the expected latency contribution is.

**Where:** `brain/src/brain/ArbDetector.cpp:242-243`

**Fix (option A — document):** Add above `fee_for_()` call:
```cpp
// fee_for_() is O(1) avg with a small (≤ 16 entry) map; acceptable at current scan rates.
```
**Fix (option B — pre-compute):** Before the outer loop, build a `std::array<double, kMaxVenues> fees` indexed parallel to `venues`; replace `fee_for_()` calls with `fees[si]` / `fees[bi]`.

---

### FIX-8 — Floating-point edge case: no guard for `mid < 1.0` in spread calc

**Severity:** HIGH (data correctness — degenerate priceTick values yield astronomical bps values)

**What:** Spread is computed as `(spread_abs / mid) * 10000.0`. The `mid <= 0.0` guard exists but not `mid < 1.0`. If a venue publishes a priceTick of 1 (e.g. price $0.000001 with 6-decimal ticks), the spread_bps can be orders of magnitude above reality and still pass the `max_spread_bps` cap if the cap is 0.

**Where:** `brain/src/brain/ArbDetector.cpp:227-230`

**Fix:** Add after the `mid <= 0.0` guard:
```cpp
if (mid < 1.0) continue; // degenerate tick — price-tick unit too small for reliable bps calc
```
Document the invariant: priceTick units are `price × 100`, so `mid < 1` means price < $0.005.

---

## MEDIUM

### FIX-9 — Whitespace not trimmed in `parse_venue_fees()` tokens

**Severity:** MEDIUM (usability — `"binance: 10"` fails to parse; common when editing config)

**Where:** `brain/include/brain/BrainCmdLine.hpp:19-21`

**Fix:** Trim leading/trailing whitespace from both venue name and fee string before parsing.

---

### FIX-10 — No validation that `min_spread_bps < max_spread_bps`

**Severity:** MEDIUM (usability — misconfigured values suppress all signals with no warning)

**Where:** `brain/include/brain/BrainCmdLine.hpp` (post-parse validation)

**Fix:**
```cpp
if (out.max_spread_bps > 0.0 && out.min_spread_bps > out.max_spread_bps)
    spdlog::warn("[brain] min-spread-bps ({}) > max-spread-bps ({}) — no crosses will emit",
                 out.min_spread_bps, out.max_spread_bps);
```

---

### FIX-11 — Unbounded fee value accepted (e.g. `binance:99999`)

**Severity:** MEDIUM (operational — extreme fee silently suppresses all signals for that venue)

**Where:** `brain/include/brain/BrainCmdLine.hpp:24-26`

**Fix:** Validate `fee <= 1000.0` (100% = 10000 bps is unreachable; cap at 1000 as sanity):
```cpp
if (fee > 1000.0) {
    spdlog::warn("[brain] venue-fees: suspiciously large fee {:.1f}bps for '{}' — accepted but verify",
                 fee, token.substr(0, colon));
}
```

---

## LOW

### FIX-12 — `memory_order_relaxed` usages undocumented

**Severity:** LOW (maintainability)

**Where:** `brain/include/brain/ArbDetector.hpp:92-93`, `exec/include/exec/ImmediateStrategy.hpp:33-35`

**Fix:** Add inline comment: `// relaxed: no ordering dependency with other memory operations`

---

### FIX-13 — `serialize_cross()` missing `lag_ns` field

**Severity:** LOW (consistency — exec clients receive cross without detection latency)

**Where:** `brain/app/brain.cpp:50-65`

**Fix:** Add `j["lag_ns"] = c.lag_ns;` (blocked by FIX-6 — consolidate serialisation first).

---

### FIX-14 — `brain.conf` venue-fees comment undated

**Severity:** LOW (docs decay)

**Where:** `config/brain.conf:26-28`

**Fix:** Add `# Last verified: 2026-Q1 VIP0 spot taker rates` above the example line.

---

## Priority Order

| # | Fix | Severity | Effort |
|---|-----|----------|--------|
| FIX-1 | E1 wiring gap (position limit no-op) | CRITICAL | 1–2 h |
| FIX-2 | Negative fee accepted | HIGH | 30 min |
| FIX-3 | Malformed token silent skip | HIGH | 30 min |
| FIX-4 | `net_spread_bps` uninitialized in test helper | HIGH | 15 min |
| FIX-5 | JSONL queue overflow silent | HIGH | 30 min |
| FIX-6 | Duplicate JSONL serialization | HIGH | 1–2 h |
| FIX-7 | Hot-path fee lookup undocumented | HIGH | 30 min (doc) / 1 h (pre-compute) |
| FIX-8 | `mid < 1.0` floating-point edge case | HIGH | 15 min |
| FIX-9 | Whitespace not trimmed in fee tokens | MEDIUM | 30 min |
| FIX-10 | No `min > max` spread validation | MEDIUM | 15 min |
| FIX-11 | Unbounded fee value | MEDIUM | 15 min |
| FIX-12 | `memory_order_relaxed` undocumented | LOW | 15 min |
| FIX-13 | `lag_ns` missing from serialize_cross | LOW | 15 min (after FIX-6) |
| FIX-14 | brain.conf comment undated | LOW | 5 min |
