#include "brain/ArbDetector.hpp"

#include <chrono>
#include <cmath>
#include <iostream>

#include <nlohmann/json.hpp>

namespace brain {

ArbDetector::ArbDetector(double min_spread_bps,
                         std::int64_t max_age_diff_ns,
                         std::string output_path)
    : min_spread_bps_(min_spread_bps),
      max_age_diff_ns_(max_age_diff_ns) {
    if (!output_path.empty()) {
        output_.open(output_path, std::ios::out | std::ios::app);
        if (!output_.is_open())
            std::cerr << "[ArbDetector] WARNING: could not open output file: " << output_path << "\n";
    }
}

std::int64_t ArbDetector::now_ns_() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

void ArbDetector::emit_(const ArbCross &c) {
    // Log to stderr
    std::cerr << "[ARB] sell=" << c.sell_venue
              << " bid=" << c.sell_bid_tick
              << " buy=" << c.buy_venue
              << " ask=" << c.buy_ask_tick
              << " spread=" << c.spread_bps << "bps\n";

    // Optionally write JSONL
    if (!output_.is_open()) return;
    try {
        nlohmann::json j;
        j["ts_detected_ns"]  = c.ts_detected_ns;
        j["sell_venue"]      = c.sell_venue;
        j["buy_venue"]       = c.buy_venue;
        j["sell_bid_tick"]   = c.sell_bid_tick;
        j["buy_ask_tick"]    = c.buy_ask_tick;
        j["spread_bps"]      = c.spread_bps;
        j["sell_ts_book_ns"] = c.sell_ts_book_ns;
        j["buy_ts_book_ns"]  = c.buy_ts_book_ns;
        output_ << j.dump() << '\n';
        output_.flush();
    } catch (...) {}
}

std::vector<ArbCross> ArbDetector::scan(const std::vector<VenueBook> &venues) {
    std::vector<ArbCross> result;
    const std::int64_t ts_now = now_ns_();

    for (std::size_t si = 0; si < venues.size(); ++si) {
        const VenueBook &sv = venues[si];
        if (!sv.synced()) continue;

        const std::int64_t sell_bid_tick = sv.book().best_bid().priceTick;
        if (sell_bid_tick == 0) continue; // empty book

        for (std::size_t bi = 0; bi < venues.size(); ++bi) {
            if (bi == si) continue;
            const VenueBook &bv = venues[bi];
            if (!bv.synced()) continue;

            const std::int64_t buy_ask_tick = bv.book().best_ask().priceTick;
            if (buy_ask_tick == 0) continue; // empty book

            // Cross condition: sell_bid > buy_ask
            if (sell_bid_tick <= buy_ask_tick) continue;

            // Individual book age guard — catches two books both stale by the same amount
            if (max_age_diff_ns_ > 0 && (ts_now - sv.ts_book_ns) > max_age_diff_ns_) continue;
            if (max_age_diff_ns_ > 0 && (ts_now - bv.ts_book_ns) > max_age_diff_ns_) continue;

            // Relative staleness guard — catches books that diverged in time
            const std::int64_t age_diff = std::abs(sv.ts_book_ns - bv.ts_book_ns);
            if (max_age_diff_ns_ > 0 && age_diff > max_age_diff_ns_) continue;

            // Spread in bps (priceTick units are price × 100; ratio is dimensionless)
            const double spread_abs = static_cast<double>(sell_bid_tick - buy_ask_tick);
            const double mid        = static_cast<double>(sell_bid_tick + buy_ask_tick) * 0.5;
            if (mid <= 0.0) continue; // guard against degenerate ticks
            const double spread_bps = (spread_abs / mid) * 10000.0;

            if (spread_bps < min_spread_bps_) continue;

            ArbCross cross;
            cross.sell_venue       = sv.venue_name;
            cross.buy_venue        = bv.venue_name;
            cross.sell_bid_tick    = sell_bid_tick;
            cross.buy_ask_tick     = buy_ask_tick;
            cross.spread_bps       = spread_bps;
            cross.ts_detected_ns   = ts_now;
            cross.sell_ts_book_ns  = sv.ts_book_ns;
            cross.buy_ts_book_ns   = bv.ts_book_ns;

            emit_(cross);
            result.push_back(std::move(cross));
        }
    }

    return result;
}

} // namespace brain
