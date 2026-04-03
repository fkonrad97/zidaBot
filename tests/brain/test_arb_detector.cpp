#include <gtest/gtest.h>
#include <chrono>

#include "brain/ArbDetector.hpp"
#include "brain/UnifiedBook.hpp"         // VenueBook
#include "orderbook/OrderBookController.hpp"

using namespace brain;

// ── helpers ───────────────────────────────────────────────────────────────────
static std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static Level lvl(std::int64_t price, std::int64_t qty) {
    return Level{price, qty, {}, {}};
}

/// Build a VenueBook in Synced+healthy state with a single bid and ask.
static VenueBook make_synced(std::string venue,
                              std::int64_t bid_tick,
                              std::int64_t ask_tick,
                              std::int64_t ts_ns) {
    VenueBook vb(venue, "BTC/USDT", 10);
    GenericSnapshotFormat snap;
    snap.lastUpdateId = 1;
    snap.ts_recv_ns   = ts_ns;
    snap.bids.push_back(lvl(bid_tick, 100));
    snap.asks.push_back(lvl(ask_tick, 100));
    vb.controller->onSnapshot(snap, md::OrderBookController::BaselineKind::WsAuthoritative);
    vb.ts_book_ns   = ts_ns;
    vb.feed_healthy = true;
    return vb;
}

/// Default permissive ArbDetector (no rate limit, no age guard, no file output).
static ArbDetector make_detector(double min_bps = 0.1,
                                  double max_bps = 0.0,
                                  std::unordered_map<std::string, double> fees = {}) {
    return ArbDetector(
        min_bps,
        max_bps,
        0,           // rate_limit_ns  = off
        0,           // max_age_diff_ns = off
        0.0,         // max_price_deviation_pct = off
        "",          // no output file
        0,           // output_max_bytes = off
        std::move(fees)
    );
}

// ── basic detection ───────────────────────────────────────────────────────────
TEST(ArbDetector, DetectsCrossWhenSellBidExceedsBuyAsk) {
    // binance bid=10100, okx ask=9900 → sell binance, buy okx
    auto det = make_detector();
    std::vector<VenueBook> venues;
    const std::int64_t ts = now_ns();
    venues.push_back(make_synced("binance", 10100, 10200, ts));
    venues.push_back(make_synced("okx",     10000, 9900,  ts));

    auto crosses = det.scan(venues);
    ASSERT_EQ(crosses.size(), 1u);
    EXPECT_EQ(crosses[0].sell_venue, "binance");
    EXPECT_EQ(crosses[0].buy_venue,  "okx");
    EXPECT_EQ(crosses[0].sell_bid_tick, 10100);
    EXPECT_EQ(crosses[0].buy_ask_tick,   9900);
    EXPECT_GT(crosses[0].spread_bps, 0.0);
}

TEST(ArbDetector, NoCrossWhenBidLessThanOrEqualAsk) {
    auto det = make_detector();
    std::vector<VenueBook> venues;
    const std::int64_t ts = now_ns();
    venues.push_back(make_synced("binance", 10000, 10100, ts));
    venues.push_back(make_synced("okx",     10000, 10050, ts));
    // binance bid=10000 vs okx ask=10050: 10000 < 10050 → no cross
    // okx bid=10000 vs binance ask=10100: 10000 < 10100 → no cross
    EXPECT_EQ(det.scan(venues).size(), 0u);
}

TEST(ArbDetector, EmptyVenueListReturnsEmpty) {
    auto det = make_detector();
    std::vector<VenueBook> venues;
    EXPECT_EQ(det.scan(venues).size(), 0u);
}

TEST(ArbDetector, SingleVenueCannotSelfArb) {
    auto det = make_detector();
    std::vector<VenueBook> venues;
    venues.push_back(make_synced("binance", 10100, 9900, now_ns()));
    // si == bi is skipped → no cross
    EXPECT_EQ(det.scan(venues).size(), 0u);
}

// ── min_spread filter ─────────────────────────────────────────────────────────
TEST(ArbDetector, CrossBelowMinSpreadSuppressed) {
    // bid=10001, ask=10000 → spread ~ 1 tick / ~10000 * 10000 = 1 bps
    ArbDetector det(500.0, 0.0, 0, 0, 0.0, ""); // require 500 bps minimum
    std::vector<VenueBook> venues;
    const std::int64_t ts = now_ns();
    venues.push_back(make_synced("a", 10001, 10100, ts));
    venues.push_back(make_synced("b", 10000, 10000, ts));
    EXPECT_EQ(det.scan(venues).size(), 0u);
}

// ── max_spread cap (B1) ───────────────────────────────────────────────────────
TEST(ArbDetector, CrossAboveMaxSpreadSuppressedAsAnomaly) {
    // bid=20000, ask=10000 → very large spread → anomaly
    ArbDetector det(0.1, 100.0, 0, 0, 0.0, ""); // cap at 100 bps
    std::vector<VenueBook> venues;
    const std::int64_t ts = now_ns();
    venues.push_back(make_synced("a", 20000, 25000, ts));
    venues.push_back(make_synced("b", 15000, 10000, ts));
    // spread_bps = (20000-10000) / ((20000+10000)*0.5) * 10000 >> 100 bps
    EXPECT_EQ(det.scan(venues).size(), 0u);
}

// ── staleness guard (max_age_diff_ns) ─────────────────────────────────────────
TEST(ArbDetector, StaleBookExcludedFromScan) {
    // Set max_age_diff_ns = 1 ns; books have ts_book_ns=0 → ancient → stale
    ArbDetector det(0.1, 0.0, 0, 1 /*ns*/, 0.0, "");
    std::vector<VenueBook> venues;
    venues.push_back(make_synced("a", 10100, 10200, 0)); // ts_ns=0 → stale
    venues.push_back(make_synced("b", 10000, 9900,  0));
    EXPECT_EQ(det.scan(venues).size(), 0u);
}

// ── feed_healthy guard ────────────────────────────────────────────────────────
TEST(ArbDetector, UnhealthyFeedExcludedFromScan) {
    auto det = make_detector();
    std::vector<VenueBook> venues;
    const std::int64_t ts = now_ns();
    venues.push_back(make_synced("a", 10100, 10200, ts));
    venues.push_back(make_synced("b", 10000, 9900,  ts));
    venues.back().feed_healthy = false; // mark unhealthy → VenueBook::synced() = false
    EXPECT_EQ(det.scan(venues).size(), 0u);
}

// ── rate limit (B2) ───────────────────────────────────────────────────────────
TEST(ArbDetector, RateLimitSuppressesSecondEmission) {
    // rate_limit_ns = 1000 seconds — effectively blocks any immediate repeat
    ArbDetector det(0.1, 0.0, 1'000'000'000'000LL, 0, 0.0, "");
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> v1, v2;
    v1.push_back(make_synced("a", 10100, 10200, ts));
    v1.push_back(make_synced("b", 10000, 9900,  ts));
    v2.push_back(make_synced("a", 10100, 10200, ts));
    v2.push_back(make_synced("b", 10000, 9900,  ts));

    auto first  = det.scan(v1);
    auto second = det.scan(v2);
    EXPECT_EQ(first.size(),  1u); // detected
    EXPECT_EQ(second.size(), 0u); // suppressed by rate limit
}

// ── price deviation filter (B5) ───────────────────────────────────────────────
TEST(ArbDetector, PriceDeviationAnomalyExcludesVenue) {
    // One venue's bid is wildly off (50% from median) — excluded
    // max_price_deviation_pct = 10%
    ArbDetector det(0.1, 0.0, 0, 0, 10.0, "");
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> venues;
    venues.push_back(make_synced("a", 10000, 10100, ts)); // normal
    venues.push_back(make_synced("b", 10050, 10150, ts)); // normal
    venues.push_back(make_synced("c", 20000, 20100, ts)); // bid=20000: far from median ~10025
    // c is flagged as price anomaly → excluded from sell side; no cross expected
    // a bid=10000 > b ask=10150? no. b bid=10050 > a ask=10100? no.
    EXPECT_EQ(det.scan(venues).size(), 0u);
}

// ── standby mode (F4): scan() still returns crosses ──────────────────────────
TEST(ArbDetector, StandbyModeStillReturnsCrossesButSuppressesEmit) {
    ArbDetector det(0.1, 0.0, 0, 0, 0.0, "");
    det.set_active(false);
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> venues;
    venues.push_back(make_synced("a", 10100, 10200, ts));
    venues.push_back(make_synced("b", 10000, 9900,  ts));
    // scan() adds to result regardless; emit_() is a no-op when !active_
    auto crosses = det.scan(venues);
    EXPECT_EQ(crosses.size(), 1u);
    // last_cross_ns_ not updated when inactive
    EXPECT_EQ(det.last_cross_ns(), 0);
}

// ── last_cross_ns updated on active emit ─────────────────────────────────────
TEST(ArbDetector, LastCrossNsUpdatedAfterActiveEmit) {
    auto det = make_detector();
    EXPECT_EQ(det.last_cross_ns(), 0);
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> venues;
    venues.push_back(make_synced("a", 10100, 10200, ts));
    venues.push_back(make_synced("b", 10000, 9900,  ts));
    det.scan(venues);
    EXPECT_GT(det.last_cross_ns(), 0);
}

// ── ES1: on_cross_ callback ───────────────────────────────────────────────────
TEST(ArbDetector, OnCrossCallbackFiredForValidCross) {
    auto det = make_detector();
    int fired = 0;
    det.on_cross_ = [&fired](const brain::ArbCross &) { ++fired; };
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> venues;
    venues.push_back(make_synced("a", 10100, 10200, ts));
    venues.push_back(make_synced("b", 10000,  9900, ts));
    det.scan(venues);
    EXPECT_EQ(fired, 1);
}

TEST(ArbDetector, OnCrossCallbackNotFiredWhenNoCross) {
    auto det = make_detector();
    int fired = 0;
    det.on_cross_ = [&fired](const brain::ArbCross &) { ++fired; };
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> venues;
    venues.push_back(make_synced("a", 10000, 10100, ts));
    venues.push_back(make_synced("b", 10050, 10200, ts));
    det.scan(venues);
    EXPECT_EQ(fired, 0);
}

TEST(ArbDetector, OnCrossCallbackNotFiredInStandbyMode) {
    ArbDetector det(0.1, 0.0, 0, 0, 0.0, "");
    det.set_active(false);
    int fired = 0;
    det.on_cross_ = [&fired](const brain::ArbCross &) { ++fired; };
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> venues;
    venues.push_back(make_synced("a", 10100, 10200, ts));
    venues.push_back(make_synced("b", 10000,  9900, ts));
    det.scan(venues);
    // scan() returns the cross but emit_() is suppressed in standby → callback must not fire
    EXPECT_EQ(fired, 0);
}

TEST(ArbDetector, OnCrossCallbackReceivesCorrectCrossFields) {
    auto det = make_detector();
    brain::ArbCross captured{};
    det.on_cross_ = [&captured](const brain::ArbCross &c) { captured = c; };
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> venues;
    venues.push_back(make_synced("sell_v", 10100, 10200, ts));
    venues.push_back(make_synced("buy_v",  10000,  9900, ts));
    det.scan(venues);
    EXPECT_EQ(captured.sell_venue, "sell_v");
    EXPECT_EQ(captured.buy_venue,  "buy_v");
    EXPECT_EQ(captured.sell_bid_tick, 10100);
    EXPECT_EQ(captured.buy_ask_tick,   9900);
    EXPECT_GT(captured.spread_bps, 0.0);
    EXPECT_GT(captured.ts_detected_ns, 0);
}

TEST(ArbDetector, OnCrossCallbackSuppressedByRateLimit) {
    // rate limit of 1000 seconds → only first scan emits
    ArbDetector det(0.1, 0.0, 1'000'000'000'000LL, 0, 0.0, "", 0);
    int fired = 0;
    det.on_cross_ = [&fired](const brain::ArbCross &) { ++fired; };
    const std::int64_t ts = now_ns();

    auto make_v = [&]() {
        std::vector<VenueBook> v;
        v.push_back(make_synced("a", 10100, 10200, ts));
        v.push_back(make_synced("b", 10000,  9900, ts));
        return v;
    };

    det.scan(make_v());
    det.scan(make_v());
    EXPECT_EQ(fired, 1); // second emission suppressed by rate limit
}

// ── fee-adjusted spread ───────────────────────────────────────────────────────

TEST(ArbDetector, FeeAdjustedSpread_SuppressedWhenNetBelowMin) {
    // sell_bid=10010, buy_ask=9990 → spread=(20/10000)*10000 = 20 bps.
    // With sell_fee=10bps + buy_fee=10bps → net = 20 - 20 = 0 bps.
    // min_spread_bps=0.1 → net(0) < min(0.1) → suppressed.
    auto det = make_detector(/*min_bps=*/0.1, /*max_bps=*/0.0,
                             {{"binance", 10.0}, {"okx", 10.0}});
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> v;
    v.push_back(make_synced("binance", 10010, 10200, ts));
    v.push_back(make_synced("okx",     10000,  9990, ts));

    auto crosses = det.scan(v);
    EXPECT_TRUE(crosses.empty());
}

TEST(ArbDetector, FeeAdjustedSpread_PassesWhenNetAboveMin) {
    // sell_bid=10010, buy_ask=9990 → raw spread = 20 bps.
    // Fees: sell=5bps + buy=5bps → net = 10 bps > min=0.1 → passes.
    auto det = make_detector(/*min_bps=*/0.1, /*max_bps=*/0.0,
                             {{"binance", 5.0}, {"okx", 5.0}});
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> v;
    v.push_back(make_synced("binance", 10010, 10200, ts));
    v.push_back(make_synced("okx",     10000,  9990, ts));

    auto crosses = det.scan(v);
    ASSERT_EQ(crosses.size(), 1u);
    EXPECT_GT(crosses[0].net_spread_bps, 0.0);
    EXPECT_LT(crosses[0].net_spread_bps, crosses[0].spread_bps); // net < raw
}

TEST(ArbDetector, FeeAdjustedSpread_MissingVenueDefaultsToZeroFee) {
    // Only okx fee configured; binance defaults to 0.
    // net = raw(20bps) - 0(binance) - 8(okx) = 12bps > min=0.1 → passes.
    auto det = make_detector(/*min_bps=*/0.1, /*max_bps=*/0.0,
                             {{"okx", 8.0}});
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> v;
    v.push_back(make_synced("binance", 10010, 10200, ts));
    v.push_back(make_synced("okx",     10000,  9990, ts));

    auto crosses = det.scan(v);
    ASSERT_EQ(crosses.size(), 1u);
    EXPECT_GT(crosses[0].net_spread_bps, 0.0);
}

TEST(ArbDetector, FeeAdjustedSpread_NetStoredInCross) {
    // Verify net_spread_bps == spread_bps - sell_fee - buy_fee.
    // sell_fee(binance)=10, buy_fee(okx)=8. Raw spread = 20 bps → net = 2 bps.
    auto det = make_detector(0.0, 0.0, {{"binance", 10.0}, {"okx", 8.0}});
    const std::int64_t ts = now_ns();
    std::vector<VenueBook> v;
    v.push_back(make_synced("binance", 10010, 10200, ts));
    v.push_back(make_synced("okx",     10000,  9990, ts));

    auto crosses = det.scan(v);
    ASSERT_EQ(crosses.size(), 1u);
    const double expected_net = crosses[0].spread_bps - 10.0 - 8.0;
    EXPECT_NEAR(crosses[0].net_spread_bps, expected_net, 1e-9);
}
