#include "exec/ExecEngine.hpp"

#include <spdlog/spdlog.h>

namespace exec {

ExecEngine::ExecEngine(std::string                    my_venue,
                       std::unique_ptr<IExecStrategy> strategy,
                       double                         position_limit,
                       double                         max_order_notional,
                       std::int64_t                   cooldown_ns)
    : my_venue_(std::move(my_venue))
    , position_limit_(position_limit)
    , max_order_notional_(max_order_notional)
    , cooldown_ns_(cooldown_ns)
    , strategy_(std::move(strategy)) {}

// Applies pre-trade guards in order E4 → E1 → E2 → E3, then dispatches to strategy.
//
// Guard rationale:
//   E4 first — cheapest check (single comparison against a config constant). Catches
//              obviously wrong prices before any state is consulted.
//   E1 second — reads open_notional_ (one double compare). Position circuit-breaker.
//   E2 third  — atomic load. Kill switch; rarely true but must be checked every signal.
//   E3 last   — unordered_map lookup + string concat. Most expensive guard, but only
//               reached by signals that passed all price/risk checks.
void ExecEngine::on_signal(const brain::ArbCross &cross) {
    // E4: fat-finger — sell_bid_tick is the larger price in any arb cross
    // (sell_bid > buy_ask always), making it the most conservative notional estimate
    // when no order quantity is available in the ArbCross signal.
    if (max_order_notional_ > 0.0) {
        const double ref_price = cross.sell_bid_tick / 100.0;
        if (ref_price > max_order_notional_) {
            spdlog::warn("[ExecEngine] E4 fat-finger: ref_price={:.2f} > limit={:.2f} sell={} buy={}",
                         ref_price, max_order_notional_,
                         cross.sell_venue, cross.buy_venue);
            return;
        }
    }

    // E1: position limit — gross cumulative notional circuit-breaker.
    // open_notional_ grows monotonically via on_fill(); once the limit is hit the
    // engine stays disabled until an operator calls resume() or restarts.
    if (position_limit_ > 0.0) {
        const double new_notional = cross.sell_bid_tick / 100.0;
        if (open_notional_ + new_notional > position_limit_) {
            spdlog::warn("[ExecEngine] E1 position limit: open={:.2f} + new={:.2f} > limit={:.2f}",
                         open_notional_, new_notional, position_limit_);
            return;
        }
    }

    // E2: kill switch — atomic load is safe from any thread; dispatch is blocked
    // while paused_ is true regardless of the source of the pause.
    if (paused_.load(std::memory_order_relaxed)) {
        spdlog::debug("[ExecEngine] E2 paused — signal suppressed sell={} buy={}",
                      cross.sell_venue, cross.buy_venue);
        return;
    }

    // E3: per-pair dedup / cooldown — suppresses burst of duplicate signals for the
    // same directed pair within cooldown_ns_. Uses brain's ts_detected_ns to avoid
    // a now() syscall on the hot path. Key is bounded: at most N*(N-1) directed pairs.
    if (cooldown_ns_ > 0) {
        const std::string key = cross.sell_venue + ":" + cross.buy_venue;
        auto it = last_signal_ns_.find(key);
        if (it != last_signal_ns_.end() &&
            (cross.ts_detected_ns - it->second) < cooldown_ns_) {
            spdlog::debug("[ExecEngine] E3 cooldown: pair={} age_ns={} < {}",
                          key,
                          cross.ts_detected_ns - it->second,
                          cooldown_ns_);
            return;
        }
        last_signal_ns_[key] = cross.ts_detected_ns;
    }

    strategy_->on_signal(cross);
}

// Accumulates filled notional into the E1 position tracker.
// Called from the strategy fill callback which already runs on the exec strand,
// so open_notional_ can be mutated directly without a lock.
void ExecEngine::on_fill(const Fill &fill) {
    open_notional_ += fill.filled_qty * (fill.fill_price_tick / 100.0);
}

// E2: atomically pause signal dispatch and forward to strategy.
// The atomic store is async-signal-safe; the virtual call is not — see header caveat.
void ExecEngine::pause() {
    paused_.store(true, std::memory_order_relaxed);
    strategy_->pause();
    spdlog::warn("[ExecEngine] E2 kill switch: PAUSED venue={}", my_venue_);
}

// E2: resume after pause. Symmetric with pause().
void ExecEngine::resume() {
    paused_.store(false, std::memory_order_relaxed);
    strategy_->resume();
    spdlog::info("[ExecEngine] E2 kill switch: RESUMED venue={}", my_venue_);
}

} // namespace exec
