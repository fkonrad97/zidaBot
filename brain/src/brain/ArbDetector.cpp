#include "brain/ArbDetector.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <spdlog/spdlog.h>
#include <vector>

#include <nlohmann/json.hpp>

namespace brain {

ArbDetector::ArbDetector(double min_spread_bps,
                         double max_spread_bps,
                         std::int64_t rate_limit_ns,
                         std::int64_t max_age_diff_ns,
                         double max_price_deviation_pct,
                         std::string output_path,
                         std::uint64_t output_max_bytes)
    : min_spread_bps_(min_spread_bps),
      max_spread_bps_(max_spread_bps),
      rate_limit_ns_(rate_limit_ns),
      max_age_diff_ns_(max_age_diff_ns),
      max_price_deviation_pct_(max_price_deviation_pct),
      output_path_base_(output_path),
      output_max_bytes_(output_max_bytes) {
    if (!output_path.empty()) {
        output_.open(output_path, std::ios::out | std::ios::app);
        if (!output_.is_open())
            spdlog::warn("[ArbDetector] could not open output file: {}", output_path);
    }
}

std::int64_t ArbDetector::now_ns_() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

bool ArbDetector::check_rate_limit_(const std::string &key, std::int64_t ts_now) noexcept {
    if (rate_limit_ns_ <= 0) return true;
    auto it = last_emit_ns_.find(key);
    if (it != last_emit_ns_.end() && (ts_now - it->second) < rate_limit_ns_) return false;
    last_emit_ns_[key] = ts_now;
    return true;
}

void ArbDetector::rotate_output_() {
    if (output_path_base_.empty()) return;
    output_.flush();
    output_.close();

    // Build rotated filename: <base>.<rotate_seq>
    ++output_rotate_seq_;
    const std::string rotated = output_path_base_ + "." + std::to_string(output_rotate_seq_);
    // Rename current file to rotated name
    std::rename(output_path_base_.c_str(), rotated.c_str());

    // Open a fresh file
    output_.open(output_path_base_, std::ios::out | std::ios::trunc);
    if (!output_.is_open()) {
        spdlog::warn("[ArbDetector] could not reopen output after rotation");
    } else {
        spdlog::info("[ArbDetector] rotated output to {} (bytes={})", rotated, output_bytes_);
    }
    output_bytes_ = 0;
}

void ArbDetector::emit_(const ArbCross &c) {
    // F4: standby mode — build state but suppress emission
    if (!active_.load(std::memory_order_relaxed)) return;

    ++crosses_total_;
    last_cross_ns_ = c.ts_detected_ns;

    // F5: record detection latency (time from freshest book update to detection)
    const std::int64_t lag_ns =
        c.ts_detected_ns - std::max(c.sell_ts_book_ns, c.buy_ts_book_ns);
    if (lag_ns > 0) latency_hist_.record(lag_ns);

    spdlog::info("[ARB] sell={} bid={} buy={} ask={} spread={}bps",
                 c.sell_venue, c.sell_bid_tick, c.buy_venue, c.buy_ask_tick, c.spread_bps);

    // ES1: notify external subscriber (e.g. SignalServer) if a callback is registered.
    // Placed after all internal guards (active_, counters, latency) so only fully
    // validated crosses reach the execution layer. The if-check is free when unset.
    if (on_cross_) on_cross_(c);

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
        j["lag_ns"]          = lag_ns; // F5: detection latency for offline analysis
        const std::string line = j.dump() + '\n';
        output_ << line;
        output_.flush();
        output_bytes_ += static_cast<std::uint64_t>(line.size());

        // D3: rotate if size limit reached
        if (output_max_bytes_ > 0 && output_bytes_ >= output_max_bytes_) {
            rotate_output_();
        }
    } catch (...) {}
}

std::vector<ArbCross> ArbDetector::scan(const std::vector<VenueBook> &venues) {
    std::vector<ArbCross> result;
    const std::int64_t ts_now = now_ns_();

    // B5: cross-venue price sanity — compute median best_bid across synced venues.
    // Any venue whose best_bid deviates by more than max_price_deviation_pct_ is
    // excluded from this scan and logged as an anomaly.
    std::vector<bool> price_anomaly(venues.size(), false);
    if (max_price_deviation_pct_ > 0.0) {
        std::vector<std::int64_t> bids;
        bids.reserve(venues.size());
        for (const auto &vb : venues) {
            if (!vb.synced()) continue;
            const std::int64_t bid = vb.book().best_bid().priceTick;
            if (bid > 0) bids.push_back(bid);
        }
        if (bids.size() >= 2) {
            std::vector<std::int64_t> sorted_bids = bids;
            std::sort(sorted_bids.begin(), sorted_bids.end());
            const double median = (sorted_bids.size() % 2 == 0)
                ? static_cast<double>(sorted_bids[sorted_bids.size() / 2 - 1]
                                    + sorted_bids[sorted_bids.size() / 2]) * 0.5
                : static_cast<double>(sorted_bids[sorted_bids.size() / 2]);

            const double threshold = median * (max_price_deviation_pct_ / 100.0);
            for (std::size_t i = 0; i < venues.size(); ++i) {
                if (!venues[i].synced()) continue;
                const std::int64_t bid = venues[i].book().best_bid().priceTick;
                if (bid > 0 && std::abs(static_cast<double>(bid) - median) > threshold) {
                    price_anomaly[i] = true;
                    spdlog::warn("[ArbDetector] B5 ANOMALY: venue={} best_bid={} median={} deviation_pct={} > {}",
                                 venues[i].venue_name, bid, static_cast<std::int64_t>(median),
                                 std::abs(static_cast<double>(bid) - median) / median * 100.0,
                                 max_price_deviation_pct_);
                }
            }
        }
    }

    for (std::size_t si = 0; si < venues.size(); ++si) {
        const VenueBook &sv = venues[si];
        if (!sv.synced()) continue;
        if (price_anomaly[si]) continue; // B5: skip anomalous venue

        const std::int64_t sell_bid_tick = sv.book().best_bid().priceTick;
        if (sell_bid_tick == 0) continue; // empty book

        for (std::size_t bi = 0; bi < venues.size(); ++bi) {
            if (bi == si) continue;
            const VenueBook &bv = venues[bi];
            if (!bv.synced()) continue;
            if (price_anomaly[bi]) continue; // B5: skip anomalous venue

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

            // B1: upper spread cap — anomalously large spreads indicate a data error
            if (max_spread_bps_ > 0.0 && spread_bps > max_spread_bps_) {
                spdlog::warn("[ArbDetector] ANOMALY: spread {}bps exceeds cap {}bps sell={} buy={}",
                             spread_bps, max_spread_bps_, sv.venue_name, bv.venue_name);
                continue;
            }

            // B2: per-pair rate limiter — suppress duplicate signals within rate_limit_ns_
            const std::string pair_key = sv.venue_name + ":" + bv.venue_name;
            if (!check_rate_limit_(pair_key, ts_now)) continue;

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
