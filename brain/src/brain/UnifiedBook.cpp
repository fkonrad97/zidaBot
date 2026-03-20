#include "brain/UnifiedBook.hpp"
#include "brain/JsonParsers.hpp"

#include <chrono>
#include <spdlog/spdlog.h>

namespace brain {

namespace {
/// Return system clock time in nanoseconds since epoch.
inline std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// Validate and sanitize ts_book_ns from PoP.
/// - Clamps future timestamps (>60 s ahead) to now to prevent the staleness
///   guard being permanently bypassed.
/// - Logs a warning on very old timestamps (>5 min behind) but keeps them so
///   ArbDetector's age guard can reject the book normally.
inline std::int64_t sanitize_ts(std::int64_t ts, std::string_view venue) noexcept {
    constexpr std::int64_t kMaxFutureNs = 60LL  * 1'000'000'000LL; // 60 s
    constexpr std::int64_t kWarnPastNs  = 300LL * 1'000'000'000LL; // 5 min
    const std::int64_t ts_now = now_ns();
    if (ts > ts_now + kMaxFutureNs) {
        spdlog::warn("[UnifiedBook] future ts_book_ns from venue={} diff=+{}ms — clamped to now",
                     venue, (ts - ts_now) / 1'000'000);
        return ts_now;
    }
    if (ts > 0 && ts < ts_now - kWarnPastNs) {
        spdlog::warn("[UnifiedBook] stale ts_book_ns from venue={} age={}ms",
                     venue, (ts_now - ts) / 1'000'000);
    }
    return ts;
}
} // namespace

// ---------------------------------------------------------------------------
// VenueBook

VenueBook::VenueBook(std::string name, std::string sym, std::size_t depth)
    : venue_name(std::move(name)),
      symbol(std::move(sym)),
      controller(std::make_unique<md::OrderBookController>(depth)) {
    // Brain joins mid-stream: allow sequence gaps on all controllers.
    // No checksum function: PoP has already validated.
    controller->setAllowSequenceGap(true);
}

bool VenueBook::synced() const noexcept {
    return feed_healthy && controller->isSynced();
}

const md::OrderBook &VenueBook::book() const noexcept {
    return controller->book();
}

// ---------------------------------------------------------------------------
// UnifiedBook

UnifiedBook::UnifiedBook(std::size_t depth) : depth_(depth) {}

VenueBook *UnifiedBook::find_or_create_(const std::string &venue, const std::string &symbol) {
    for (auto &vb : books_) {
        if (vb.venue_name == venue && vb.symbol == symbol) return &vb;
    }
    books_.emplace_back(venue, symbol, depth_);
    spdlog::info("[UnifiedBook] registered new venue+symbol: {}:{}", venue, symbol);
    return &books_.back();
}

std::size_t UnifiedBook::synced_count() const noexcept {
    std::size_t n = 0;
    for (const auto &vb : books_)
        if (vb.synced()) ++n;
    return n;
}

std::string UnifiedBook::on_event(const nlohmann::json &j) {
    EventHeader hdr;
    try {
        hdr = parse_header(j);
    } catch (...) {
        return {};
    }

    if (hdr.venue.empty() || hdr.event_type.empty()) return {};

    VenueBook *vb = find_or_create_(hdr.venue, hdr.symbol);

    try {
        if (hdr.event_type == "snapshot") {
            auto snap = parse_snapshot(j);
            vb->ts_book_ns   = sanitize_ts(snap.ts_recv_ns, hdr.venue);
            vb->feed_healthy = true;
            const auto action = vb->controller->onSnapshot(
                snap, md::OrderBookController::BaselineKind::WsAuthoritative);
            if (action == md::OrderBookController::Action::NeedResync) {
                spdlog::warn("[UnifiedBook] NeedResync after snapshot venue={} — book cleared, awaiting next event",
                             hdr.venue);
                vb->feed_healthy = false; // don't use this venue until resync succeeds
            }

        } else if (hdr.event_type == "incremental") {
            auto inc = parse_incremental(j);
            vb->ts_book_ns   = sanitize_ts(inc.ts_recv_ns, hdr.venue);
            vb->feed_healthy = true;
            const auto action = vb->controller->onIncrement(inc);
            if (action == md::OrderBookController::Action::NeedResync)
                spdlog::warn("[UnifiedBook] NeedResync after incremental venue={} — awaiting next book_state",
                             hdr.venue);

        } else if (hdr.event_type == "book_state") {
            vb->ts_book_ns   = sanitize_ts(extract_ts_book_ns(j), hdr.venue);
            vb->feed_healthy = true;
            auto snap = parse_book_state_as_snapshot(j);
            const auto action = vb->controller->onSnapshot(
                snap, md::OrderBookController::BaselineKind::WsAuthoritative);
            if (action == md::OrderBookController::Action::NeedResync)
                spdlog::warn("[UnifiedBook] NeedResync after book_state venue={}", hdr.venue);

        } else if (hdr.event_type == "status") {
            const std::string feed_state = j.value("feed_state", "");
            const std::string reason     = j.value("reason", "");
            if (reason.empty())
                spdlog::info("[UnifiedBook] status venue={} state={}", hdr.venue, feed_state);
            else
                spdlog::info("[UnifiedBook] status venue={} state={} reason={}", hdr.venue, feed_state, reason);

            if (feed_state == "disconnected") {
                // PoP lost its exchange feed — book is stale; stop using it immediately.
                vb->feed_healthy = false;
                vb->controller->resetBook();
                vb->ts_book_ns = 0;
            } else {
                // "resyncing" or any unknown transitional state: mark unhealthy until
                // the next data event confirms the feed is live again.
                vb->feed_healthy = false;
            }

        } else {
            return {};
        }
    } catch (const std::exception &e) {
        spdlog::warn("[UnifiedBook] parse error venue={} type={} err={}", hdr.venue, hdr.event_type, e.what());
        return {};
    } catch (...) {
        spdlog::warn("[UnifiedBook] unknown parse error venue={}", hdr.venue);
        return {};
    }

    return hdr.venue;
}

} // namespace brain
