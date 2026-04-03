#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include "exec/DeadlineOrderTracker.hpp"
#include "exec/IOrderClient.hpp"

using namespace std::chrono_literals;

// All DeadlineOrderTracker methods must be called from the exec strand.
// Pattern: post a lambda onto the strand, then drain via ioc.run_for().

static constexpr auto kTimeout = 20ms;   // short so tests finish quickly
static constexpr auto kDrain   = 100ms;  // enough for timer + callback round-trip

// ── helper ────────────────────────────────────────────────────────────────────

static exec::Order make_order(std::string oid, std::string venue = "binance")
{
    exec::Order o;
    o.client_order_id = std::move(oid);
    o.venue           = std::move(venue);
    o.side            = exec::Side::BUY;
    o.price_tick      = 50000;
    o.quantity        = 0.01;
    return o;
}

static exec::Fill make_fill(const std::string &oid)
{
    exec::Fill f;
    f.client_order_id = oid;
    f.filled_qty      = 0.01;
    f.fill_price_tick = 50000;
    f.ts_ns           = 0;
    return f;
}

// ── tests ─────────────────────────────────────────────────────────────────────

TEST(DeadlineOrderTracker, FillBeforeTimeout_TimerCancelled) {
    boost::asio::io_context ioc;
    auto strand = boost::asio::make_strand(ioc);

    int timeout_calls = 0;
    auto tracker = exec::DeadlineOrderTracker::create(
        strand, kTimeout,
        [&timeout_calls](const std::string &) { ++timeout_calls; });

    boost::asio::post(strand, [&] {
        tracker->register_pending(make_order("oid-1"));
        tracker->on_fill(make_fill("oid-1")); // fill arrives immediately
    });

    ioc.run_for(kDrain);

    EXPECT_EQ(tracker->pending_count(), 0u);
    EXPECT_EQ(timeout_calls, 0);
}

TEST(DeadlineOrderTracker, TimeoutFires_WhenNoFillArrives) {
    boost::asio::io_context ioc;
    auto strand = boost::asio::make_strand(ioc);

    int         timeout_calls = 0;
    std::string timeout_oid;
    auto tracker = exec::DeadlineOrderTracker::create(
        strand, kTimeout,
        [&](const std::string &oid) { ++timeout_calls; timeout_oid = oid; });

    boost::asio::post(strand, [&] {
        tracker->register_pending(make_order("oid-2"));
    });

    // Run longer than timeout so the timer fires
    ioc.run_for(kTimeout * 3);

    EXPECT_EQ(tracker->pending_count(), 0u);
    EXPECT_EQ(timeout_calls, 1);
    EXPECT_EQ(timeout_oid, "oid-2");
}

TEST(DeadlineOrderTracker, CancelAll_ClearsAllPending) {
    boost::asio::io_context ioc;
    auto strand = boost::asio::make_strand(ioc);

    int timeout_calls = 0;
    auto tracker = exec::DeadlineOrderTracker::create(
        strand, kTimeout,
        [&timeout_calls](const std::string &) { ++timeout_calls; });

    boost::asio::post(strand, [&] {
        tracker->register_pending(make_order("oid-a"));
        tracker->register_pending(make_order("oid-b"));
        tracker->register_pending(make_order("oid-c"));
        tracker->cancel_all();
    });

    ioc.run_for(kDrain);

    EXPECT_EQ(tracker->pending_count(), 0u);
    EXPECT_EQ(timeout_calls, 0); // timers cancelled — no timeouts fired
}

TEST(DeadlineOrderTracker, UnknownFillOid_IsNoop) {
    boost::asio::io_context ioc;
    auto strand = boost::asio::make_strand(ioc);

    auto tracker = exec::DeadlineOrderTracker::create(strand, kTimeout);

    boost::asio::post(strand, [&] {
        tracker->register_pending(make_order("real-oid"));
        tracker->on_fill(make_fill("ghost-oid")); // unknown — should not crash
    });

    // Drain just long enough for the post to execute, but less than kTimeout
    // so the real-oid timer hasn't fired yet.
    ioc.run_for(1ms);

    // "real-oid" still pending (no fill received for it, timer not yet expired)
    EXPECT_EQ(tracker->pending_count(), 1u);

    // Clean up before timer fires
    boost::asio::post(strand, [&] { tracker->cancel_all(); });
    ioc.run_for(kDrain);
}

TEST(DeadlineOrderTracker, PendingCount_TracksCorrectly) {
    boost::asio::io_context ioc;
    auto strand = boost::asio::make_strand(ioc);

    auto tracker = exec::DeadlineOrderTracker::create(strand, kTimeout);

    boost::asio::post(strand, [&] {
        tracker->register_pending(make_order("p1"));
        tracker->register_pending(make_order("p2"));
        tracker->register_pending(make_order("p3"));
    });
    ioc.run_for(1ms); // let the post execute

    // Drain only the posted lambdas (not the timers)
    // pending_count is safe to read from main thread after strand has settled
    // because ioc.run_for() returns when no more ready work exists.
    EXPECT_EQ(tracker->pending_count(), 3u);

    boost::asio::post(strand, [&] { tracker->on_fill(make_fill("p1")); });
    ioc.run_for(1ms);
    EXPECT_EQ(tracker->pending_count(), 2u);

    boost::asio::post(strand, [&] { tracker->on_fill(make_fill("p2")); });
    ioc.run_for(1ms);
    EXPECT_EQ(tracker->pending_count(), 1u);

    // Clean up remaining
    boost::asio::post(strand, [&] { tracker->cancel_all(); });
    ioc.run_for(kDrain);
    EXPECT_EQ(tracker->pending_count(), 0u);
}

TEST(DeadlineOrderTracker, MultipleTimeouts_AllCallbacksFire) {
    boost::asio::io_context ioc;
    auto strand = boost::asio::make_strand(ioc);

    int timeout_calls = 0;
    auto tracker = exec::DeadlineOrderTracker::create(
        strand, kTimeout,
        [&timeout_calls](const std::string &) { ++timeout_calls; });

    boost::asio::post(strand, [&] {
        tracker->register_pending(make_order("x1"));
        tracker->register_pending(make_order("x2"));
    });

    ioc.run_for(kTimeout * 3);

    EXPECT_EQ(tracker->pending_count(), 0u);
    EXPECT_EQ(timeout_calls, 2);
}

TEST(DeadlineOrderTracker, FillAfterTimeout_OidAlreadyEvicted_IsNoop) {
    boost::asio::io_context ioc;
    auto strand = boost::asio::make_strand(ioc);

    auto tracker = exec::DeadlineOrderTracker::create(strand, kTimeout);

    boost::asio::post(strand, [&] {
        tracker->register_pending(make_order("late-oid"));
    });

    // Let the timer fire and evict the oid
    ioc.run_for(kTimeout * 3);
    EXPECT_EQ(tracker->pending_count(), 0u);

    // Now send a fill for the already-evicted oid — must not crash
    boost::asio::post(strand, [&] {
        tracker->on_fill(make_fill("late-oid"));
    });

    ioc.run_for(kDrain);
    EXPECT_EQ(tracker->pending_count(), 0u);
}
