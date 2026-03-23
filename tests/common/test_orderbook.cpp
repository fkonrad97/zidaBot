#include <gtest/gtest.h>
#include "orderbook/OrderBook.hpp"

// ── helpers ───────────────────────────────────────────────────────────────────
static Level lvl(std::int64_t price, std::int64_t qty) {
    return Level{price, qty, std::to_string(price), std::to_string(qty)};
}

// ── construction ──────────────────────────────────────────────────────────────
TEST(OrderBook, ZeroDepthThrows) {
    EXPECT_THROW(md::OrderBook{0}, std::invalid_argument);
}

TEST(OrderBook, EmptyBookReturnsSentinelBBO) {
    md::OrderBook book{10};
    EXPECT_EQ(book.best_bid().priceTick, 0);
    EXPECT_EQ(book.best_ask().priceTick, 0);
    EXPECT_TRUE(book.best_bid().isEmpty());
    EXPECT_TRUE(book.best_ask().isEmpty());
}

// ── insert / best-price ───────────────────────────────────────────────────────
TEST(OrderBook, BidsSortedDescending) {
    md::OrderBook book{5};
    book.update<Side::BID>(lvl(100, 10));
    book.update<Side::BID>(lvl(102, 5));
    book.update<Side::BID>(lvl(101, 7));

    EXPECT_EQ(book.best_bid().priceTick, 102); // highest bid first
    EXPECT_EQ(book.bid_ptr(1)->priceTick, 101);
    EXPECT_EQ(book.bid_ptr(2)->priceTick, 100);
}

TEST(OrderBook, AsksSortedAscending) {
    md::OrderBook book{5};
    book.update<Side::ASK>(lvl(200, 10));
    book.update<Side::ASK>(lvl(198, 5));
    book.update<Side::ASK>(lvl(199, 7));

    EXPECT_EQ(book.best_ask().priceTick, 198); // lowest ask first
    EXPECT_EQ(book.ask_ptr(1)->priceTick, 199);
    EXPECT_EQ(book.ask_ptr(2)->priceTick, 200);
}

// ── update (quantity change) ──────────────────────────────────────────────────
TEST(OrderBook, UpdateExistingLevelChangesQty) {
    md::OrderBook book{5};
    book.update<Side::BID>(lvl(100, 10));
    book.update<Side::BID>(lvl(100, 99));
    EXPECT_EQ(book.best_bid().quantityLot, 99);
}

// ── remove ────────────────────────────────────────────────────────────────────
TEST(OrderBook, RemovePresentLevel) {
    md::OrderBook book{5};
    book.update<Side::BID>(lvl(100, 10));
    book.update<Side::BID>(lvl(102, 5));
    book.remove<Side::BID>(102);
    EXPECT_EQ(book.best_bid().priceTick, 100);
}

TEST(OrderBook, RemoveNonExistentLevelIsNoop) {
    md::OrderBook book{5};
    book.update<Side::BID>(lvl(100, 10));
    EXPECT_NO_THROW(book.remove<Side::BID>(999));
    EXPECT_EQ(book.best_bid().priceTick, 100);
}

TEST(OrderBook, UpdateWithZeroQtyActsAsRemove) {
    md::OrderBook book{5};
    book.update<Side::ASK>(lvl(200, 10));
    book.update<Side::ASK>(lvl(200, 0)); // qty=0 → remove
    EXPECT_EQ(book.best_ask().priceTick, 0); // empty sentinel
}

// ── depth enforcement ─────────────────────────────────────────────────────────
TEST(OrderBook, DepthLimitDropsWorstLevel) {
    md::OrderBook book{2};
    book.update<Side::BID>(lvl(100, 1));
    book.update<Side::BID>(lvl(102, 1));
    book.update<Side::BID>(lvl(98,  1)); // worst bid — below depth-2 frontier, dropped
    EXPECT_EQ(book.bid_ptr(0)->priceTick, 102);
    EXPECT_EQ(book.bid_ptr(1)->priceTick, 100);
    EXPECT_EQ(book.bid_ptr(2), nullptr); // only 2 levels kept
}

TEST(OrderBook, AskDepthLimitDropsWorstLevel) {
    md::OrderBook book{2};
    book.update<Side::ASK>(lvl(200, 1));
    book.update<Side::ASK>(lvl(198, 1));
    book.update<Side::ASK>(lvl(202, 1)); // worst ask — beyond depth-2 frontier, dropped
    EXPECT_EQ(book.ask_ptr(0)->priceTick, 198);
    EXPECT_EQ(book.ask_ptr(1)->priceTick, 200);
    EXPECT_EQ(book.ask_ptr(2), nullptr);
}

// ── clear ─────────────────────────────────────────────────────────────────────
TEST(OrderBook, ClearEmptiesBothSides) {
    md::OrderBook book{5};
    book.update<Side::BID>(lvl(100, 10));
    book.update<Side::ASK>(lvl(200, 10));
    book.clear();
    EXPECT_EQ(book.best_bid().priceTick, 0);
    EXPECT_EQ(book.best_ask().priceTick, 0);
    EXPECT_EQ(book.bid_ptr(0), nullptr);
    EXPECT_EQ(book.ask_ptr(0), nullptr);
}

// ── validate ──────────────────────────────────────────────────────────────────
TEST(OrderBook, ValidatePassesOnWellFormedBook) {
    md::OrderBook book{5};
    book.update<Side::BID>(lvl(102, 1));
    book.update<Side::BID>(lvl(100, 1));
    book.update<Side::ASK>(lvl(200, 1));
    book.update<Side::ASK>(lvl(202, 1));
    EXPECT_TRUE(book.validate());
}

TEST(OrderBook, ValidatePassesOnEmptyBook) {
    md::OrderBook book{5};
    EXPECT_TRUE(book.validate());
}

// ── ptr helpers ───────────────────────────────────────────────────────────────
TEST(OrderBook, PtrOutOfBoundsReturnsNull) {
    md::OrderBook book{5};
    EXPECT_EQ(book.bid_ptr(0), nullptr);
    EXPECT_EQ(book.ask_ptr(10), nullptr);
}
