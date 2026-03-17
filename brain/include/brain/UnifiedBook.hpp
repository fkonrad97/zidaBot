#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "orderbook/OrderBook.hpp"
#include "orderbook/OrderBookController.hpp"

namespace brain {

/// Holds a live OrderBook for one (venue, symbol) pair, maintained by an OrderBookController.
struct VenueBook {
    std::string venue_name; ///< e.g. "binance"
    std::string symbol;     ///< e.g. "BTCUSDT" — combined with venue_name as the lookup key
    std::unique_ptr<md::OrderBookController> controller;

    /// Timestamp of the last update (ts_book_ns from book_state, ts_recv_ns from others).
    /// Used by ArbDetector's staleness guard.
    std::int64_t ts_book_ns{0};

    /// True only when PoP has explicitly reported its feed as healthy (via a status
    /// event or by successfully delivering book data). Cleared immediately on a
    /// "disconnected" status so the book is excluded from arb scanning even if the
    /// controller's internal state still shows Synced from the previous cycle.
    bool feed_healthy{false};

    VenueBook(std::string venue, std::string symbol, std::size_t depth);

    // Non-copyable (unique_ptr member)
    VenueBook(const VenueBook &)            = delete;
    VenueBook &operator=(const VenueBook &) = delete;
    VenueBook(VenueBook &&)                 = default;
    VenueBook &operator=(VenueBook &&)      = default;

    /// Returns true only when both the controller is Synced AND PoP's feed is healthy.
    [[nodiscard]] bool synced() const noexcept;
    [[nodiscard]] const md::OrderBook &book() const noexcept;
};

/// Maintains one VenueBook per venue and routes incoming JSON events.
class UnifiedBook {
public:
    explicit UnifiedBook(std::size_t depth);

    /// Route one JSON event (snapshot / incremental / book_state) to the
    /// appropriate VenueBook. Returns the updated venue name, or "" if the
    /// event was discarded (unknown type, parse error, missing venue field).
    std::string on_event(const nlohmann::json &j);

    [[nodiscard]] const std::vector<VenueBook> &venues() const noexcept { return books_; }
    [[nodiscard]] std::size_t synced_count() const noexcept;

private:
    std::size_t depth_;
    std::vector<VenueBook> books_; ///< at most N_venues entries; linear lookup is fine

    VenueBook *find_or_create_(const std::string &venue, const std::string &symbol);
};

} // namespace brain
