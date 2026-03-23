#include <gtest/gtest.h>
#include "orderbook/OrderBookController.hpp"

using OBC  = md::OrderBookController;
using Kind = OBC::BaselineKind;

// ── helpers ───────────────────────────────────────────────────────────────────
static Level lvl(std::int64_t price, std::int64_t qty) {
    return Level{price, qty, std::to_string(price), std::to_string(qty)};
}

static GenericSnapshotFormat make_snap(std::uint64_t seq,
                                       std::int64_t  bid_tick,
                                       std::int64_t  ask_tick) {
    GenericSnapshotFormat s;
    s.lastUpdateId = seq;
    s.ts_recv_ns   = 0;
    s.bids.push_back(lvl(bid_tick, 100));
    s.asks.push_back(lvl(ask_tick, 100));
    return s;
}

static GenericIncrementalFormat make_inc(std::uint64_t first,
                                         std::uint64_t last) {
    GenericIncrementalFormat inc;
    inc.first_seq  = first;
    inc.last_seq   = last;
    inc.ts_recv_ns = 0;
    return inc;
}

// ── initial state ─────────────────────────────────────────────────────────────
TEST(OrderBookController, InitiallyNotSynced) {
    OBC c{10};
    EXPECT_FALSE(c.isSynced());
    EXPECT_EQ(c.getSyncState(), OBC::BookSyncState::WaitingSnapshot);
}

// ── WsAuthoritative snapshot ──────────────────────────────────────────────────
TEST(OrderBookController, WsAuthoritativeSnapshotSyncsImmediately) {
    OBC c{10};
    auto snap = make_snap(100, 10000, 10100);
    EXPECT_EQ(c.onSnapshot(snap, Kind::WsAuthoritative), OBC::Action::None);
    EXPECT_TRUE(c.isSynced());
    EXPECT_EQ(c.getAppliedSeqID(), 100u);
    EXPECT_EQ(c.book().best_bid().priceTick, 10000);
    EXPECT_EQ(c.book().best_ask().priceTick, 10100);
}

// ── RestAnchored snapshot + bridge ───────────────────────────────────────────
TEST(OrderBookController, RestAnchoredSnapshotEntersWaitingBridge) {
    OBC c{10};
    auto snap = make_snap(100, 10000, 10100);
    EXPECT_EQ(c.onSnapshot(snap, Kind::RestAnchored), OBC::Action::None);
    EXPECT_EQ(c.getSyncState(), OBC::BookSyncState::WaitingBridge);
    EXPECT_FALSE(c.isSynced());
}

TEST(OrderBookController, BridgingIncrementalCompletesSyncing) {
    OBC c{10};
    c.onSnapshot(make_snap(100, 10000, 10100), Kind::RestAnchored);
    // expected_seq_ = 101; incremental first=101 covers it → bridge applied → Synced
    auto inc = make_inc(101, 102);
    EXPECT_EQ(c.onIncrement(inc), OBC::Action::None);
    EXPECT_TRUE(c.isSynced());
}

// ── steady-state incremental ──────────────────────────────────────────────────
TEST(OrderBookController, SteadyStateIncrementalAppliedAndSeqAdvances) {
    OBC c{10};
    c.onSnapshot(make_snap(100, 10000, 10100), Kind::WsAuthoritative);
    // expected_seq_ = 101
    auto inc = make_inc(101, 101);
    inc.bids.push_back(lvl(10050, 5)); // improve bid
    EXPECT_EQ(c.onIncrement(inc), OBC::Action::None);
    EXPECT_EQ(c.getAppliedSeqID(), 101u);
    EXPECT_EQ(c.book().best_bid().priceTick, 10050);
}

// ── sequence gap → NeedResync ─────────────────────────────────────────────────
TEST(OrderBookController, SequenceGapTriggersResync) {
    OBC c{10};
    c.onSnapshot(make_snap(100, 10000, 10100), Kind::WsAuthoritative);
    // expected_seq_=101; send first_seq=103 → gap
    auto inc = make_inc(103, 104);
    inc.last_seq = 104; // has_seq = true
    EXPECT_EQ(c.onIncrement(inc), OBC::Action::NeedResync);
    EXPECT_FALSE(c.isSynced());
}

// ── book crossing → NeedResync ────────────────────────────────────────────────
TEST(OrderBookController, CrossedBookAfterIncrementalTriggersResync) {
    OBC c{10};
    c.onSnapshot(make_snap(100, 10000, 10100), Kind::WsAuthoritative);
    // Apply incremental that lowers ask below bid (crossing)
    auto inc = make_inc(101, 101);
    inc.asks.push_back(lvl(9900, 50)); // ask=9900 < bid=10000 → crossed
    EXPECT_EQ(c.onIncrement(inc), OBC::Action::NeedResync);
    EXPECT_FALSE(c.isSynced());
}

// ── resetBook ────────────────────────────────────────────────────────────────
TEST(OrderBookController, ResetBookReturnsToWaitingSnapshot) {
    OBC c{10};
    c.onSnapshot(make_snap(100, 10000, 10100), Kind::WsAuthoritative);
    EXPECT_TRUE(c.isSynced());
    c.resetBook();
    EXPECT_FALSE(c.isSynced());
    EXPECT_EQ(c.getSyncState(), OBC::BookSyncState::WaitingSnapshot);
    EXPECT_EQ(c.getAppliedSeqID(), 0u);
}

// ── outdated incremental ignored ─────────────────────────────────────────────
TEST(OrderBookController, OutdatedIncrementalIgnored) {
    OBC c{10};
    c.onSnapshot(make_snap(100, 10000, 10100), Kind::WsAuthoritative);
    // last_seq_ = 100, expected_seq_ = 101; send last_seq=99 → too old
    auto inc = make_inc(98, 99);
    EXPECT_EQ(c.onIncrement(inc), OBC::Action::None);
    EXPECT_TRUE(c.isSynced()); // still synced; no state change
}

// ── allow_seq_gap tolerates gaps ─────────────────────────────────────────────
TEST(OrderBookController, AllowSeqGapToleratesGap) {
    OBC c{10};
    c.setAllowSequenceGap(true);
    c.onSnapshot(make_snap(100, 10000, 10100), Kind::WsAuthoritative);
    auto inc = make_inc(105, 106); // gap: expected 101, got 105
    EXPECT_EQ(c.onIncrement(inc), OBC::Action::None);
    EXPECT_TRUE(c.isSynced());
}

// ── B7: zero-qty levels in snapshot skipped ──────────────────────────────────
TEST(OrderBookController, SnapshotZeroQtyLevelsSkipped) {
    OBC c{10};
    GenericSnapshotFormat s;
    s.lastUpdateId = 1;
    s.bids.push_back(Level{10000, 0, "", ""}); // zero qty → should be skipped
    s.asks.push_back(Level{10100, 50, "", ""});
    c.onSnapshot(s, Kind::WsAuthoritative);
    EXPECT_EQ(c.book().best_bid().priceTick, 0); // no bid applied
    EXPECT_EQ(c.book().best_ask().priceTick, 10100);
}

// ── incremental in WaitingSnapshot returns None (handler buffers) ──────────
TEST(OrderBookController, IncrementalBeforeSnapshotReturnsNone) {
    OBC c{10};
    // seq-less incremental in WaitingSnapshot state — controller says None
    GenericIncrementalFormat inc;
    inc.last_seq  = 0; // no seq
    inc.first_seq = 0;
    EXPECT_EQ(c.onIncrement(inc), OBC::Action::None);
    EXPECT_FALSE(c.isSynced());
}