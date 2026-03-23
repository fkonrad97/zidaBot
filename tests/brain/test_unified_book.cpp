#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "brain/UnifiedBook.hpp"
#include "brain/JsonParsers.hpp"

using namespace brain;
using json = nlohmann::json;

// ── JSON fixture builders ─────────────────────────────────────────────────────
static json make_level(std::int64_t price, std::int64_t qty) {
    return {{"priceTick", price}, {"quantityLot", qty}, {"price", ""}, {"quantity", ""}};
}

static json make_snapshot(const std::string &venue,
                           std::int64_t bid_tick = 10000,
                           std::int64_t ask_tick = 10100,
                           std::uint64_t seq     = 1) {
    return {
        {"schema_version", 1},
        {"event_type",     "snapshot"},
        {"venue",          venue},
        {"symbol",         "BTCUSDT"},
        {"seq_last",       seq},
        {"ts_recv_ns",     0},
        {"checksum",       0},
        {"bids",           json::array({make_level(bid_tick, 100)})},
        {"asks",           json::array({make_level(ask_tick, 100)})}
    };
}

static json make_incremental(const std::string &venue,
                              std::uint64_t first,
                              std::uint64_t last,
                              std::int64_t  new_bid_tick = 0) {
    json inc = {
        {"schema_version", 1},
        {"event_type",     "incremental"},
        {"venue",          venue},
        {"symbol",         "BTCUSDT"},
        {"seq_first",      first},
        {"seq_last",       last},
        {"prev_last",      first - 1},
        {"ts_recv_ns",     0},
        {"checksum",       0},
        {"bids",           json::array()},
        {"asks",           json::array()}
    };
    if (new_bid_tick > 0)
        inc["bids"].push_back(make_level(new_bid_tick, 200));
    return inc;
}

static json make_status(const std::string &venue, const std::string &state) {
    return {
        {"schema_version", 1},
        {"event_type",     "status"},
        {"venue",          venue},
        {"symbol",         "BTCUSDT"},
        {"feed_state",     state}
    };
}

// ── basic routing ─────────────────────────────────────────────────────────────
TEST(UnifiedBook, SnapshotCreatesVenueAndReturnVenueName) {
    UnifiedBook book{10};
    const std::string result = book.on_event(make_snapshot("binance"));
    EXPECT_EQ(result, "binance");
    EXPECT_EQ(book.venues().size(), 1u);
}

TEST(UnifiedBook, TwoDistinctVenuesTrackedSeparately) {
    UnifiedBook book{10};
    book.on_event(make_snapshot("binance"));
    book.on_event(make_snapshot("okx"));
    EXPECT_EQ(book.venues().size(), 2u);
}

TEST(UnifiedBook, SameVenueNotDuplicated) {
    UnifiedBook book{10};
    book.on_event(make_snapshot("binance", 10000, 10100, 1));
    book.on_event(make_snapshot("binance", 10050, 10150, 2)); // second snapshot, same venue
    EXPECT_EQ(book.venues().size(), 1u);
}

// ── synced_count ──────────────────────────────────────────────────────────────
TEST(UnifiedBook, SyncedCountAfterSnapshot) {
    UnifiedBook book{10};
    EXPECT_EQ(book.synced_count(), 0u);
    book.on_event(make_snapshot("binance"));
    EXPECT_EQ(book.synced_count(), 1u);
    book.on_event(make_snapshot("okx"));
    EXPECT_EQ(book.synced_count(), 2u);
}

// ── incremental updates book ──────────────────────────────────────────────────
TEST(UnifiedBook, IncrementalUpdatesBestBid) {
    UnifiedBook book{10};
    // ask=10500 leaves room for a bid improvement to 10200 without crossing
    book.on_event(make_snapshot("binance", 10000, 10500, 1));
    // VenueBook has allowSeqGap=true, so incremental with any seq is applied
    book.on_event(make_incremental("binance", 2, 2, 10200)); // improve bid (10200 < 10500 ask)
    const auto &vb = book.venues().front();
    EXPECT_EQ(vb.book().best_bid().priceTick, 10200);
}

// ── status events ─────────────────────────────────────────────────────────────
TEST(UnifiedBook, DisconnectedStatusMarksFeedUnhealthy) {
    UnifiedBook book{10};
    book.on_event(make_snapshot("binance"));
    EXPECT_EQ(book.synced_count(), 1u);
    book.on_event(make_status("binance", "disconnected"));
    EXPECT_EQ(book.synced_count(), 0u); // feed_healthy = false
}

TEST(UnifiedBook, ResyncingStatusMarksFeedUnhealthy) {
    UnifiedBook book{10};
    book.on_event(make_snapshot("binance"));
    book.on_event(make_status("binance", "resyncing"));
    EXPECT_EQ(book.synced_count(), 0u);
}

TEST(UnifiedBook, SnapshotAfterDisconnectRestoresSynced) {
    UnifiedBook book{10};
    book.on_event(make_snapshot("binance", 10000, 10100, 1));
    book.on_event(make_status("binance", "disconnected"));
    EXPECT_EQ(book.synced_count(), 0u);
    book.on_event(make_snapshot("binance", 10000, 10100, 2));
    EXPECT_EQ(book.synced_count(), 1u);
}

// ── malformed events ──────────────────────────────────────────────────────────
TEST(UnifiedBook, UnknownEventTypeReturnsEmpty) {
    UnifiedBook book{10};
    json j = {
        {"schema_version", 1},
        {"event_type",     "unknown_type"},
        {"venue",          "binance"},
        {"symbol",         "BTCUSDT"}
    };
    // find_or_create_ runs before the type check, so a VenueBook entry is allocated,
    // but on_event returns "" and the book is left in WaitingSnapshot (not synced).
    EXPECT_EQ(book.on_event(j), "");
    EXPECT_EQ(book.synced_count(), 0u);
}

TEST(UnifiedBook, WrongSchemaVersionReturnsEmpty) {
    UnifiedBook book{10};
    json j = make_snapshot("binance");
    j["schema_version"] = 2; // unsupported
    EXPECT_EQ(book.on_event(j), "");
}

TEST(UnifiedBook, MissingVenueFieldReturnsEmpty) {
    UnifiedBook book{10};
    json j = {
        {"schema_version", 1},
        {"event_type",     "snapshot"},
        {"symbol",         "BTCUSDT"},
        // no "venue" key
        {"seq_last",  1},
        {"ts_recv_ns", 0},
        {"bids", json::array()},
        {"asks", json::array()}
    };
    EXPECT_EQ(book.on_event(j), "");
}

// ── JsonParsers inline helpers ────────────────────────────────────────────────
TEST(JsonParsers, ParseHeaderHappyPath) {
    json j = {
        {"schema_version", 1},
        {"event_type",     "snapshot"},
        {"venue",          "okx"},
        {"symbol",         "ETHUSDT"}
    };
    const auto hdr = parse_header(j);
    EXPECT_EQ(hdr.schema_version, 1);
    EXPECT_EQ(hdr.event_type, "snapshot");
    EXPECT_EQ(hdr.venue,      "okx");
    EXPECT_EQ(hdr.symbol,     "ETHUSDT");
}

TEST(JsonParsers, ParseHeaderWrongVersionThrows) {
    json j = {{"schema_version", 99}, {"event_type", ""}, {"venue", ""}, {"symbol", ""}};
    EXPECT_THROW(parse_header(j), std::runtime_error);
}

TEST(JsonParsers, ParseLevelHappyPath) {
    json j = {{"priceTick", 10000}, {"quantityLot", 50}, {"price", "100.00"}, {"quantity", "0.5"}};
    const auto l = parse_level(j);
    EXPECT_EQ(l.priceTick,   10000);
    EXPECT_EQ(l.quantityLot, 50);
}

TEST(JsonParsers, ParseLevelNegativeQtyThrows) {
    json j = {{"priceTick", 10000}, {"quantityLot", -1}, {"price", ""}, {"quantity", ""}};
    EXPECT_THROW(parse_level(j), std::runtime_error);
}

TEST(JsonParsers, ParseLevelZeroPriceOnRestingLevelThrows) {
    json j = {{"priceTick", 0}, {"quantityLot", 10}, {"price", ""}, {"quantity", ""}};
    EXPECT_THROW(parse_level(j), std::runtime_error);
}

TEST(JsonParsers, ParseLevelZeroPriceWithZeroQtyIsRemoveSignal) {
    // qty=0 is a remove signal; priceTick=0 is allowed
    json j = {{"priceTick", 0}, {"quantityLot", 0}, {"price", ""}, {"quantity", ""}};
    EXPECT_NO_THROW(parse_level(j));
}
