#pragma once

// Header-only JSON → C++ struct deserialization for brain.
// Field names must match exactly what WsPublishSink serializes.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "orderbook/OrderBook.hpp"
#include "orderbook/OrderBookController.hpp"

namespace brain {

struct EventHeader {
    int         schema_version{0};
    std::string event_type; ///< "snapshot" | "incremental" | "book_state"
    std::string venue;
    std::string symbol;
};

inline EventHeader parse_header(const nlohmann::json &j) {
    EventHeader h;
    h.schema_version = j.value("schema_version", 0);
    if (h.schema_version != 1)
        throw std::runtime_error("unsupported schema_version: " + std::to_string(h.schema_version));
    h.event_type     = j.value("event_type", "");
    h.venue          = j.value("venue", "");
    h.symbol         = j.value("symbol", "");
    return h;
}

/// Parse a single level object. priceTick/quantityLot are already integers
/// in the wire format (pre-computed by WsPublishSink); no conversion needed.
inline Level parse_level(const nlohmann::json &lvl) {
    Level l;
    l.priceTick   = lvl.value("priceTick",   std::int64_t{0});
    l.quantityLot = lvl.value("quantityLot", std::int64_t{0});
    l.price       = lvl.value("price",    std::string{});
    l.quantity    = lvl.value("quantity", std::string{});
    return l;
}

inline std::vector<Level> parse_levels(const nlohmann::json &arr) {
    std::vector<Level> out;
    if (!arr.is_array()) return out;
    out.reserve(arr.size());
    for (const auto &item : arr)
        out.push_back(parse_level(item));
    return out;
}

/// Parse event_type == "snapshot"
inline GenericSnapshotFormat parse_snapshot(const nlohmann::json &j) {
    GenericSnapshotFormat s;
    // seq_first == seq_last in the snapshot wire format (single lastUpdateId limitation)
    s.lastUpdateId = j.value("seq_last",   std::uint64_t{0});
    s.ts_recv_ns   = j.value("ts_recv_ns", std::int64_t{0});
    s.checksum     = j.value("checksum",   std::int64_t{0});
    s.bids         = parse_levels(j.at("bids"));
    s.asks         = parse_levels(j.at("asks"));
    return s;
}

/// Parse event_type == "incremental"
inline GenericIncrementalFormat parse_incremental(const nlohmann::json &j) {
    GenericIncrementalFormat inc;
    inc.first_seq  = j.value("seq_first",  std::uint64_t{0});
    inc.last_seq   = j.value("seq_last",   std::uint64_t{0});
    inc.prev_last  = j.value("prev_last",  std::uint64_t{0});
    inc.ts_recv_ns = j.value("ts_recv_ns", std::int64_t{0});
    inc.checksum   = j.value("checksum",   std::int64_t{0});
    inc.bids       = parse_levels(j.at("bids"));
    inc.asks       = parse_levels(j.at("asks"));
    return inc;
}

/// Parse event_type == "book_state" as a snapshot for WsAuthoritative resync.
/// Sets lastUpdateId = applied_seq; bids/asks from the checkpoint levels.
inline GenericSnapshotFormat parse_book_state_as_snapshot(const nlohmann::json &j) {
    GenericSnapshotFormat s;
    s.lastUpdateId = j.value("applied_seq", std::uint64_t{0});
    s.ts_recv_ns   = j.value("ts_book_ns",  std::int64_t{0});
    s.checksum     = 0; // not stored in book_state; PoP already validated
    s.bids         = parse_levels(j.at("bids"));
    s.asks         = parse_levels(j.at("asks"));
    return s;
}

/// Extract the book-state event timestamp for VenueBook::ts_book_ns tracking.
inline std::int64_t extract_ts_book_ns(const nlohmann::json &j) {
    return j.value("ts_book_ns", std::int64_t{0});
}

} // namespace brain
