#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "brain/ArbDetector.hpp"
#include "brain/UnifiedBook.hpp"

namespace brain {

/// Synchronous wrapper around UnifiedBook + ArbDetector for replaying historical
/// JSONL event files from PoP's --persist_path output.
///
/// No io_context, no threads, no TLS, no file I/O. Caller owns all I/O.
class BacktestEngine {
public:
    /// @param depth                    Order book depth per venue
    /// @param min_spread_bps           Suppress crosses below this threshold (0 = all)
    /// @param max_spread_bps           Suppress crosses above this threshold (0 = no cap)
    /// @param rate_limit_ns            Min ns between signals per (sell,buy) pair (0 = unlimited)
    /// @param max_age_ns               Individual + relative book staleness guard in nanoseconds
    /// @param max_price_deviation_pct  B5: % deviation from median before venue is excluded (0 = off)
    BacktestEngine(std::size_t  depth                   = 50,
                   double       min_spread_bps           = 0.0,
                   double       max_spread_bps           = 0.0,
                   std::int64_t rate_limit_ns            = 0,
                   std::int64_t max_age_ns               = 5'000'000'000LL,
                   double       max_price_deviation_pct  = 0.0);

    /// Feed one raw JSON line (one line from a .jsonl or .jsonl.gz file).
    /// Returns all arb crosses detected as a result of this event (empty if none).
    std::vector<ArbCross> feed_event(std::string_view json_line);

    /// Name of the venue updated by the most recent feed_event() call.
    /// Empty string if the last event was discarded (parse error, unknown type, etc.).
    [[nodiscard]] const std::string &last_updated_venue() const noexcept { return last_updated_; }

    /// Number of currently synced venues.
    [[nodiscard]] std::size_t synced_count() const noexcept;

    /// Top-of-book bid/ask for a named venue. All zeros if the venue is not synced.
    struct BBO {
        std::int64_t bid_tick{0};
        std::int64_t ask_tick{0};
        std::int64_t ts_ns{0};
    };
    [[nodiscard]] BBO bbo(const std::string &venue) const noexcept;

    /// One price level: integer tick price and integer lot quantity.
    struct Level {
        std::int64_t price_tick{0};
        std::int64_t qty_lot{0};
    };

    /// Full depth snapshot for a named venue (up to min(n, depth_) levels per side).
    /// n == 0 returns all levels up to the configured depth.
    /// Returns empty bids/asks if the venue is unknown or not synced.
    struct BookDepth {
        std::string              venue;
        std::int64_t             ts_ns{0};
        std::vector<Level>       bids;  ///< sorted descending by price_tick
        std::vector<Level>       asks;  ///< sorted ascending  by price_tick
    };
    [[nodiscard]] BookDepth levels(const std::string &venue, std::size_t n = 0) const noexcept;

    /// Reset all order book and detector state (start a fresh replay window).
    void reset();

private:
    std::size_t  depth_;
    double       min_spread_bps_;
    double       max_spread_bps_;
    std::int64_t rate_limit_ns_;
    std::int64_t max_age_ns_;
    double       max_price_deviation_pct_;

    std::string last_updated_;  ///< venue name from the last successful feed_event()

    // unique_ptr avoids move-assignment issues (ArbDetector has atomic<bool> member)
    std::unique_ptr<UnifiedBook> book_;
    std::unique_ptr<ArbDetector> arb_;
};

} // namespace brain
