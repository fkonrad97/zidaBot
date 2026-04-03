#include <gtest/gtest.h>

#include "brain/ArbDetector.hpp"
#include "exec/ExecEngine.hpp"
#include "exec/IExecStrategy.hpp"
#include "exec/IOrderClient.hpp"

// ── mock ──────────────────────────────────────────────────────────────────────

struct MockStrategy : exec::IExecStrategy {
    int  calls       = 0;
    bool paused_flag = false;

    void on_signal(const brain::ArbCross &) override { ++calls; }
    void pause()  override { paused_flag = true;  }
    void resume() override { paused_flag = false; }
    [[nodiscard]] bool is_paused() const noexcept override { return paused_flag; }
};

// ── helpers ───────────────────────────────────────────────────────────────────

/// Build an ArbCross with sensible defaults. sell_bid_tick is in price-tick
/// units (price × 100), so 50000 == $500.00.
static brain::ArbCross make_cross(std::string          sell,
                                   std::string          buy,
                                   std::int64_t         sell_bid_tick = 50000,
                                   std::int64_t         ts_ns         = 1'000'000'000LL)
{
    brain::ArbCross c;
    c.sell_venue      = std::move(sell);
    c.buy_venue       = std::move(buy);
    c.sell_bid_tick   = sell_bid_tick;
    c.buy_ask_tick    = sell_bid_tick - 100; // small positive spread
    c.spread_bps      = 2.0;
    c.ts_detected_ns  = ts_ns;
    c.sell_ts_book_ns = ts_ns - 1000;
    c.buy_ts_book_ns  = ts_ns - 1000;
    return c;
}

/// Construct an ExecEngine with a fresh MockStrategy.
/// mock_out is set to the raw pointer so tests can inspect it after the
/// engine takes ownership.
static exec::ExecEngine make_engine(MockStrategy        *&mock_out,
                                    double               position_limit    = 0.0,
                                    double               max_order_notional = 0.0,
                                    std::int64_t         cooldown_ns       = 0)
{
    auto mock = std::make_unique<MockStrategy>();
    mock_out  = mock.get();
    return exec::ExecEngine("binance",
                            std::move(mock),
                            position_limit,
                            max_order_notional,
                            cooldown_ns);
}

/// Build a Fill for use with on_fill().
static exec::Fill make_fill(double qty, std::int64_t price_tick)
{
    exec::Fill f;
    f.client_order_id = "test-oid";
    f.filled_qty      = qty;
    f.fill_price_tick = price_tick;
    f.ts_ns           = 0;
    return f;
}

// ── E4: fat-finger cap ────────────────────────────────────────────────────────

TEST(ExecEngine, E4_BlocksWhenRefPriceExceedsLimit) {
    // sell_bid_tick=60000 → ref_price=$600; limit=$500 → rejected
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock, /*position_limit=*/0.0, /*max_order_notional=*/500.0);

    engine.on_signal(make_cross("binance", "okx", /*sell_bid_tick=*/60000));

    EXPECT_EQ(mock->calls, 0);
}

TEST(ExecEngine, E4_PassesWhenRefPriceBelowLimit) {
    // sell_bid_tick=40000 → ref_price=$400; limit=$500 → passes
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock, 0.0, 500.0);

    engine.on_signal(make_cross("binance", "okx", 40000));

    EXPECT_EQ(mock->calls, 1);
}

TEST(ExecEngine, E4_DisabledWhenZero) {
    // max_order_notional=0 → guard off; any price passes
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock, 0.0, 0.0);

    engine.on_signal(make_cross("binance", "okx", 9'999'999));

    EXPECT_EQ(mock->calls, 1);
}

// ── E1: position limit ────────────────────────────────────────────────────────

TEST(ExecEngine, E1_PassesWhenBelowPositionLimit) {
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock, /*position_limit=*/1000.0);

    // open_notional_ starts at 0; signal ref_price=$500 → 0+500 ≤ 1000
    engine.on_signal(make_cross("binance", "okx", 50000));

    EXPECT_EQ(mock->calls, 1);
}

TEST(ExecEngine, E1_BlocksWhenPositionLimitBreached) {
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock, /*position_limit=*/600.0);

    // Prime open_notional_ to 500 via on_fill (qty=1, price=$500)
    engine.on_fill(make_fill(1.0, 50000));

    // Next signal ref_price=$500 → 500+500=1000 > 600 → blocked
    engine.on_signal(make_cross("binance", "okx", 50000));

    EXPECT_EQ(mock->calls, 0);
}

TEST(ExecEngine, E1_DisabledWhenZero) {
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock, /*position_limit=*/0.0);

    // Prime a large notional
    engine.on_fill(make_fill(1000.0, 9'999'999));
    // Guard is off — signal always passes
    engine.on_signal(make_cross("binance", "okx", 50000));

    EXPECT_EQ(mock->calls, 1);
}

// ── E2: kill switch ───────────────────────────────────────────────────────────

TEST(ExecEngine, E2_InitiallyUnpaused) {
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock);

    engine.on_signal(make_cross("binance", "okx"));

    EXPECT_EQ(mock->calls, 1);
}

TEST(ExecEngine, E2_PauseBlocksSignal) {
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock);

    engine.pause();
    engine.on_signal(make_cross("binance", "okx"));

    EXPECT_EQ(mock->calls, 0);
}

TEST(ExecEngine, E2_ResumeAllowsSignal) {
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock);

    engine.pause();
    engine.resume();
    engine.on_signal(make_cross("binance", "okx"));

    EXPECT_EQ(mock->calls, 1);
}

// ── E3: cooldown ──────────────────────────────────────────────────────────────

TEST(ExecEngine, E3_CooldownSuppressesSecondSignal) {
    MockStrategy *mock = nullptr;
    // cooldown = 1 second = 1'000'000'000 ns
    auto engine = make_engine(mock, 0.0, 0.0, /*cooldown_ns=*/1'000'000'000LL);

    // First signal at t=1s
    engine.on_signal(make_cross("binance", "okx", 50000, /*ts=*/1'000'000'000LL));
    // Second signal 500 ms later — still inside cooldown
    engine.on_signal(make_cross("binance", "okx", 50000, /*ts=*/1'500'000'000LL));

    EXPECT_EQ(mock->calls, 1);
}

TEST(ExecEngine, E3_PassesAfterCooldownExpires) {
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock, 0.0, 0.0, 1'000'000'000LL);

    // First signal at t=1s
    engine.on_signal(make_cross("binance", "okx", 50000, 1'000'000'000LL));
    // Second signal 1.5 s later — cooldown elapsed
    engine.on_signal(make_cross("binance", "okx", 50000, 2'500'000'000LL));

    EXPECT_EQ(mock->calls, 2);
}

TEST(ExecEngine, E3_DifferentPairsAreIndependent) {
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock, 0.0, 0.0, 1'000'000'000LL);

    const std::int64_t t = 1'000'000'000LL;
    // binance→okx at t
    engine.on_signal(make_cross("binance", "okx", 50000, t));
    // okx→binance at same t — different directed pair, should pass
    engine.on_signal(make_cross("okx", "binance", 50000, t));

    EXPECT_EQ(mock->calls, 2);
}

TEST(ExecEngine, E3_DisabledWhenZero) {
    MockStrategy *mock = nullptr;
    auto engine = make_engine(mock, 0.0, 0.0, /*cooldown_ns=*/0);

    const std::int64_t t = 1'000'000'000LL;
    engine.on_signal(make_cross("binance", "okx", 50000, t));
    engine.on_signal(make_cross("binance", "okx", 50000, t)); // same ts, no cooldown
    engine.on_signal(make_cross("binance", "okx", 50000, t));

    EXPECT_EQ(mock->calls, 3);
}

// ── on_fill ───────────────────────────────────────────────────────────────────

TEST(ExecEngine, OnFill_AccumulatesNotional) {
    MockStrategy *mock = nullptr;
    // position_limit=10000 so it never blocks the fill-effect test
    auto engine = make_engine(mock, 10000.0);

    // Fill: qty=2.0, price_tick=50000 → notional = 2.0 × (50000/100) = $1000
    engine.on_fill(make_fill(2.0, 50000));

    // Now prime would block a $600 signal (1000+600=1600 > limit?).
    // Use a limit of 1500 to verify: 1000+500=1500 ≤ 1500 → passes
    MockStrategy *mock2 = nullptr;
    auto engine2 = make_engine(mock2, 1500.0);
    engine2.on_fill(make_fill(2.0, 50000)); // open_notional_=1000
    engine2.on_signal(make_cross("binance", "okx", 50000)); // ref=500, 1000+500=1500 ≤ 1500 → passes
    EXPECT_EQ(mock2->calls, 1);

    // Verify it blocks at the boundary
    MockStrategy *mock3 = nullptr;
    auto engine3 = make_engine(mock3, 1499.0);
    engine3.on_fill(make_fill(2.0, 50000)); // open_notional_=1000
    engine3.on_signal(make_cross("binance", "okx", 50000)); // 1000+500=1500 > 1499 → blocked
    EXPECT_EQ(mock3->calls, 0);
}

// ── guard ordering ────────────────────────────────────────────────────────────

TEST(ExecEngine, GuardOrder_E4FiresBeforeE1) {
    MockStrategy *mock = nullptr;
    // Both E4 (max_order_notional=$500) and E1 (position_limit=$1, already at limit) active.
    // E4 fires first because sell_bid_tick/100=$600 > $500.
    auto engine = make_engine(mock,
                              /*position_limit=*/1.0,
                              /*max_order_notional=*/500.0);

    // Signal with ref_price=$600 — E4 rejects before E1 is even checked.
    engine.on_signal(make_cross("binance", "okx", 60000));

    EXPECT_EQ(mock->calls, 0);
}
