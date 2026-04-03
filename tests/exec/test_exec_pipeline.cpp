#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include "brain/ArbDetector.hpp"
#include "exec/DeadlineOrderTracker.hpp"
#include "exec/ExecEngine.hpp"
#include "exec/ImmediateStrategy.hpp"
#include "exec/StubOrderClient.hpp"

using namespace std::chrono_literals;

// ── Pipeline fixture ──────────────────────────────────────────────────────────
//
// Wires ExecEngine → ImmediateStrategy → StubOrderClient + DeadlineOrderTracker
// on a single Asio strand, exactly as exec.cpp does it.
//
// StubOrderClient posts the fill callback asynchronously onto the strand so
// ioc.run_for() is needed to drain the fill path.

struct Pipeline {
    using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;

    boost::asio::io_context ioc;
    Strand                  strand{boost::asio::make_strand(ioc)};

    // Capture submitted orders for inspection
    std::vector<exec::Order> submitted_orders;

    // Intercept StubOrderClient by wrapping it — track orders before forwarding
    // to the real stub (which fires the fill).
    //
    // We do this with a thin capturing IOrderClient wrapper.
    struct TrackingClient : exec::IOrderClient {
        exec::StubOrderClient              inner;
        std::vector<exec::Order>          *log;

        TrackingClient(Strand s, std::vector<exec::Order> *l)
            : inner(s), log(l) {}

        void submit_order(const exec::Order &o,
                          std::function<void(std::error_code, exec::Fill)> cb) override {
            log->push_back(o);
            inner.submit_order(o, std::move(cb));
        }
        void cancel_order(const std::string &oid,
                          std::function<void(std::error_code)> cb) override {
            inner.cancel_order(oid, std::move(cb));
        }
    };

    TrackingClient                          client;
    std::shared_ptr<exec::DeadlineOrderTracker> tracker;
    std::unique_ptr<exec::ExecEngine>       engine;

    explicit Pipeline(std::string venue = "binance",
                      std::chrono::milliseconds timeout = 50ms,
                      double position_limit = 0.0,
                      double target_qty = 0.01)
        : client(strand, &submitted_orders)
    {
        tracker = exec::DeadlineOrderTracker::create(strand, timeout);

        auto strategy = std::make_unique<exec::ImmediateStrategy>(
            venue,
            client,
            *tracker,
            [this](const exec::Fill &fill) { engine->on_fill(fill); },
            strand,
            target_qty);

        engine = std::make_unique<exec::ExecEngine>(
            venue,
            std::move(strategy),
            position_limit,
            /*max_order_notional=*/0.0,
            /*cooldown_ns=*/0);
    }

    /// Post a signal onto the strand (matches production dispatch pattern).
    void send(const brain::ArbCross &cross) {
        boost::asio::post(strand, [this, cross] { engine->on_signal(cross); });
    }

    void drain(std::chrono::milliseconds d = 20ms) { ioc.run_for(d); }
};

// ── helpers ───────────────────────────────────────────────────────────────────

static brain::ArbCross make_cross(std::string sell, std::string buy,
                                   std::int64_t sell_bid_tick = 50000,
                                   std::int64_t buy_ask_tick  = 49900)
{
    brain::ArbCross c;
    c.sell_venue      = std::move(sell);
    c.buy_venue       = std::move(buy);
    c.sell_bid_tick   = sell_bid_tick;
    c.buy_ask_tick    = buy_ask_tick;
    c.spread_bps      = 2.0;
    c.ts_detected_ns  = 1'000'000'000LL;
    c.sell_ts_book_ns = c.ts_detected_ns - 1000;
    c.buy_ts_book_ns  = c.ts_detected_ns - 1000;
    return c;
}

// ── tests ─────────────────────────────────────────────────────────────────────

TEST(ExecPipeline, BuyLeg_SelectedForBuyVenue) {
    Pipeline p("binance");

    // binance is the buy venue → ImmediateStrategy should submit a BUY order
    p.send(make_cross("okx", "binance", /*sell_bid=*/50000, /*buy_ask=*/49900));
    p.drain();

    ASSERT_EQ(p.submitted_orders.size(), 1u);
    EXPECT_EQ(p.submitted_orders[0].side,       exec::Side::BUY);
    EXPECT_EQ(p.submitted_orders[0].price_tick, 49900); // buy_ask_tick
    EXPECT_EQ(p.submitted_orders[0].venue,      "binance");
}

TEST(ExecPipeline, SellLeg_SelectedForSellVenue) {
    Pipeline p("binance");

    // binance is the sell venue → SELL order at sell_bid_tick
    p.send(make_cross("binance", "okx", /*sell_bid=*/50000, /*buy_ask=*/49900));
    p.drain();

    ASSERT_EQ(p.submitted_orders.size(), 1u);
    EXPECT_EQ(p.submitted_orders[0].side,       exec::Side::SELL);
    EXPECT_EQ(p.submitted_orders[0].price_tick, 50000); // sell_bid_tick
    EXPECT_EQ(p.submitted_orders[0].venue,      "binance");
}

TEST(ExecPipeline, NeitherLeg_NoOrderSubmitted) {
    Pipeline p("bybit");

    // Signal is binance:okx — bybit is not involved
    p.send(make_cross("binance", "okx"));
    p.drain();

    EXPECT_TRUE(p.submitted_orders.empty());
    EXPECT_EQ(p.tracker->pending_count(), 0u);
}

TEST(ExecPipeline, FillArrivesBeforeTimeout_TrackerClears) {
    Pipeline p("binance");

    // StubOrderClient fires fill asynchronously on strand — one drain() cycle
    // is enough for: signal → order submitted → fill callback → tracker.on_fill()
    p.send(make_cross("okx", "binance"));
    p.drain();

    EXPECT_EQ(p.tracker->pending_count(), 0u);
}

TEST(ExecPipeline, OrderId_UniquePerSignal) {
    Pipeline p("binance");

    p.send(make_cross("okx", "binance", 50000, 49900));
    p.send(make_cross("okx", "binance", 50000, 49900));
    p.drain();

    ASSERT_EQ(p.submitted_orders.size(), 2u);
    EXPECT_NE(p.submitted_orders[0].client_order_id,
              p.submitted_orders[1].client_order_id);
}

TEST(ExecPipeline, E1PositionLimitUpdatedThroughLiveFillPath) {
    // First signal is allowed: open_notional_ starts at 0, ref_price=$500 <= $900 limit.
    // The fill callback then records ~499 USD of filled notional (qty=1.0 at 49900 tick).
    // Second signal should be blocked end-to-end because 499 + 500 > 900.
    Pipeline p("binance", /*timeout=*/500ms, /*position_limit=*/900.0, /*target_qty=*/1.0);

    p.send(make_cross("okx", "binance", /*sell_bid=*/50000, /*buy_ask=*/49900));
    p.drain();

    ASSERT_EQ(p.submitted_orders.size(), 1u);

    p.send(make_cross("okx", "binance", /*sell_bid=*/50000, /*buy_ask=*/49900));
    p.drain();

    EXPECT_EQ(p.submitted_orders.size(), 1u);
    EXPECT_EQ(p.tracker->pending_count(), 0u);
}
