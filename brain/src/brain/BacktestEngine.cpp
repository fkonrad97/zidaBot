#include "brain/BacktestEngine.hpp"

#include <nlohmann/json.hpp>

namespace brain {

static std::unique_ptr<ArbDetector> make_arb(double min_spread_bps,
                                              double max_spread_bps,
                                              std::int64_t rate_limit_ns,
                                              std::int64_t max_age_ns,
                                              double max_price_deviation_pct) {
    return std::make_unique<ArbDetector>(
        min_spread_bps, max_spread_bps, rate_limit_ns, max_age_ns,
        max_price_deviation_pct, /*output_path=*/"", /*output_max_bytes=*/0);
}

BacktestEngine::BacktestEngine(std::size_t  depth,
                               double       min_spread_bps,
                               double       max_spread_bps,
                               std::int64_t rate_limit_ns,
                               std::int64_t max_age_ns,
                               double       max_price_deviation_pct)
    : depth_(depth)
    , min_spread_bps_(min_spread_bps)
    , max_spread_bps_(max_spread_bps)
    , rate_limit_ns_(rate_limit_ns)
    , max_age_ns_(max_age_ns)
    , max_price_deviation_pct_(max_price_deviation_pct)
    , book_(std::make_unique<UnifiedBook>(depth))
    , arb_(make_arb(min_spread_bps, max_spread_bps, rate_limit_ns,
                    max_age_ns, max_price_deviation_pct))
{}

std::vector<ArbCross> BacktestEngine::feed_event(std::string_view json_line) {
    last_updated_.clear();
    try {
        const auto j = nlohmann::json::parse(json_line);
        last_updated_ = book_->on_event(j);
        if (!last_updated_.empty() && book_->synced_count() >= 2)
            return arb_->scan(book_->venues());
    } catch (...) {}
    return {};
}

std::size_t BacktestEngine::synced_count() const noexcept {
    return book_->synced_count();
}

BacktestEngine::BBO BacktestEngine::bbo(const std::string &venue) const noexcept {
    for (const auto &vb : book_->venues()) {
        if (vb.venue_name == venue && vb.synced()) {
            const auto &b = vb.book();
            return BBO{b.best_bid().priceTick, b.best_ask().priceTick, vb.ts_book_ns};
        }
    }
    return BBO{};
}

BacktestEngine::BookDepth BacktestEngine::levels(const std::string &venue,
                                                  std::size_t n) const noexcept {
    const std::size_t limit = (n == 0) ? depth_ : std::min(n, depth_);
    for (const auto &vb : book_->venues()) {
        if (vb.venue_name == venue && vb.synced()) {
            const auto &ob = vb.book();
            BookDepth bd;
            bd.venue  = venue;
            bd.ts_ns  = vb.ts_book_ns;
            for (std::size_t i = 0; i < limit; ++i) {
                const auto *b = ob.bid_ptr(i);
                if (!b) break;
                bd.bids.push_back({b->priceTick, b->quantityLot});
            }
            for (std::size_t i = 0; i < limit; ++i) {
                const auto *a = ob.ask_ptr(i);
                if (!a) break;
                bd.asks.push_back({a->priceTick, a->quantityLot});
            }
            return bd;
        }
    }
    return {venue, 0, {}, {}};
}

void BacktestEngine::reset() {
    book_ = std::make_unique<UnifiedBook>(depth_);
    arb_  = make_arb(min_spread_bps_, max_spread_bps_, rate_limit_ns_,
                     max_age_ns_, max_price_deviation_pct_);
}

} // namespace brain
