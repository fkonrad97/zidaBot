#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "brain/UnifiedBook.hpp"

namespace brain {

struct ArbCross {
    std::string sell_venue;       ///< venue where we sell (their bid > other's ask)
    std::string buy_venue;        ///< venue where we buy
    std::int64_t sell_bid_tick;   ///< sell_venue best_bid.priceTick
    std::int64_t buy_ask_tick;    ///< buy_venue best_ask.priceTick
    double       spread_bps;
    std::int64_t ts_detected_ns;
    std::int64_t sell_ts_book_ns; ///< age of the sell-venue book at detection time
    std::int64_t buy_ts_book_ns;  ///< age of the buy-venue book at detection time
};

/// Scans all directed venue pairs for cross-venue arbitrage opportunities.
/// Direct translation of unified_arb_stream.py::detect_crosses.
class ArbDetector {
public:
    ArbDetector(double min_spread_bps, std::int64_t max_age_diff_ns, std::string output_path);

    /// Scan all directed (sell, buy) venue pairs. Emits each cross to stderr
    /// and optionally to the output JSONL file. Returns all detected crosses.
    std::vector<ArbCross> scan(const std::vector<VenueBook> &venues);

    /// Flush the output file. Call before shutdown to prevent partial-line loss.
    void flush() noexcept { if (output_.is_open()) output_.flush(); }

private:
    static std::int64_t now_ns_() noexcept;
    void emit_(const ArbCross &cross);

    double        min_spread_bps_;
    std::int64_t  max_age_diff_ns_;
    std::ofstream output_;
};

} // namespace brain
