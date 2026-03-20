#pragma once

#include <cstdint>
#include <fstream>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
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
    /// @param min_spread_bps          Lower bound — crosses below this are suppressed (0 = all)
    /// @param max_spread_bps          Upper cap  — crosses above this are logged as anomalies
    ///                                and suppressed (0 = no cap); useful as a data-quality circuit breaker
    /// @param rate_limit_ns           Minimum nanoseconds between consecutive signals for the same
    ///                                (sell_venue, buy_venue) pair (0 = unlimited)
    /// @param max_age_diff_ns         Individual and relative book age guard
    /// @param max_price_deviation_pct B5: maximum allowed % deviation of a venue's best_bid from
    ///                                the median across all synced venues (0 = disabled)
    /// @param output_path             Optional JSONL output file path
    /// @param output_max_bytes        D3: rotate output file after this many bytes (0 = no rotation)
    ArbDetector(double min_spread_bps,
                double max_spread_bps,
                std::int64_t rate_limit_ns,
                std::int64_t max_age_diff_ns,
                double max_price_deviation_pct,
                std::string output_path,
                std::uint64_t output_max_bytes = 0);

    /// Scan all directed (sell, buy) venue pairs. Emits each cross to stderr
    /// and optionally to the output JSONL file. Returns all detected crosses.
    std::vector<ArbCross> scan(const std::vector<VenueBook> &venues);

    /// Flush the output file. Call before shutdown to prevent partial-line loss.
    void flush() noexcept {
        if (output_.is_open()) output_.flush();
        spdlog::info("[ArbDetector] total crosses emitted: {}", crosses_total_);
    }

    /// D4: timestamp of the last emitted cross (0 if none yet).
    [[nodiscard]] std::int64_t last_cross_ns() const noexcept { return last_cross_ns_; }

private:
    static std::int64_t now_ns_() noexcept;

    /// Returns true if the signal should be emitted (rate limit not exceeded).
    /// Updates last_emit_ns_ on return true.
    bool check_rate_limit_(const std::string &key, std::int64_t ts_now) noexcept;

    void emit_(const ArbCross &cross);

    /// D3: rotate output file when size limit is reached.
    void rotate_output_();

    double        min_spread_bps_;
    double        max_spread_bps_;        ///< 0 = no upper cap
    std::int64_t  rate_limit_ns_;         ///< 0 = no rate limit
    std::int64_t  max_age_diff_ns_;
    double        max_price_deviation_pct_; ///< B5: 0 = disabled
    std::ofstream output_;
    std::string   output_path_base_;      ///< D3: base path for rotation
    std::uint64_t output_max_bytes_{0};   ///< D3: 0 = no rotation
    std::uint64_t output_bytes_{0};       ///< D3: bytes written to current file
    std::uint32_t output_rotate_seq_{0};  ///< D3: rotation counter for unique filenames

    std::unordered_map<std::string, std::int64_t> last_emit_ns_; ///< keyed "sell:buy"
    std::uint64_t crosses_total_{0};
    std::int64_t  last_cross_ns_{0};   ///< D4: timestamp of the last emitted cross
};

} // namespace brain
