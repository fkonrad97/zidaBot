#include <string>
#include <nlohmann/json.hpp>

#include "utils/VenueUtils.hpp"
#include "md/VenueAdapter.hpp"
#include "orderbook/OrderBookUtils.hpp"
#include "utils/DebugConfigUtils.hpp"
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace md {
    VenueCaps BinanceAdapter::caps() const noexcept {
        VenueCaps c;
        c.sync_mode = SyncMode::RestAnchored;
        c.ws_sends_snapshot = false;
        c.has_checksum = false;
        c.can_backfill = false;
        return c;
    }

    /**
     * - Binance: target often encodes topic, so cfg.symbol should already be lowercased concat
     * - Example; you likely already generate a complete topic in cfg.ws_path or cfg.symbol
     * - Common: "/ws/btcusdt@depth@100ms"
     */
    EndPoint BinanceAdapter::wsEndpoint(const FeedHandlerConfig &cfg) const {
        EndPoint e;
        const std::string sym = venue::map_ws_symbol(VenueId::BINANCE, cfg.base_ccy, cfg.quote_ccy);

        // Defaults
        e.host = cfg.ws_host.empty() ? "stream.binance.com" : cfg.ws_host;

        // Binance “classic” WS endpoint is :9443
        // (You *can* use :443 on some endpoints depending on setup, but 9443 is the standard for stream.binance.com)
        e.port = cfg.ws_port.empty() ? "9443" : cfg.ws_port;

        // Target: either explicit override, or derive from base/quote
        if (!cfg.ws_path.empty()) {
            e.target = cfg.ws_path; // e.g. "/ws/btcusdt@depth@100ms"
        } else {
            e.target = "/ws/" + sym + "@depth@100ms";
        }

        return e;
    }

    EndPoint BinanceAdapter::restEndpoint(const FeedHandlerConfig &cfg) const {
        EndPoint e;
        e.host = cfg.rest_host.empty() ? "api.binance.com" : cfg.rest_host;
        e.port = cfg.rest_port.empty() ? "443" : cfg.rest_port;

        // Note: for REST you usually keep target per-request (your RestClient expects it),
        // so you can keep this empty here, and use restSnapshotTarget() for the actual request target.
        e.target = cfg.rest_path; // optionally allow override, else empty
        return e;
    }

    std::string BinanceAdapter::wsSubscribeFrame(const FeedHandlerConfig &cfg) const {
        return {};
    }

    /**
     * - Binance REST symbol is typically uppercase concat (BTCUSDT)
     */
    std::string BinanceAdapter::restSnapshotTarget(const FeedHandlerConfig &cfg) const {
        // Binance REST symbol is uppercase concat "BTCUSDT"
        const std::string rest_sym = venue::map_rest_symbol(VenueId::BINANCE, cfg.base_ccy, cfg.quote_ccy);

        // Binance depth limit must be one of allowed values; if you enforce later, do it upstream.
        return "/api/v3/depth?symbol=" + rest_sym + "&limit=" + std::to_string(cfg.depthLevel);
    }

    bool BinanceAdapter::isIncremental(std::string_view msg) const noexcept {
        return msg.find("depthUpdate") != std::string_view::npos;
    }

    bool BinanceAdapter::parseIncremental(std::string_view msg, GenericIncrementalFormat &update) const {
        update.reset();

        json j = json::parse(msg.begin(), msg.end(), nullptr, false);
        if (j.is_discarded()) {
            if (debug::dbg_on()) {
                spdlog::debug("[BINANCE][SNAPSHOT] json parse failed");
                debug::dbg_raw(msg);
            }
            return false;
        }

        if (!j.contains("e") || j["e"] != "depthUpdate") return false;
        if (!j.contains("U") || !j.contains("u") || !j.contains("b") || !j.contains("a")) return false;

        update.first_seq = j["U"].get<std::uint64_t>();
        update.last_seq = j["u"].get<std::uint64_t>();
        const auto fallback_prev = (update.last_seq > 0) ? (update.last_seq - 1) : 0;
        update.prev_last = j.value("pu", fallback_prev);

        const auto &b = j["b"];
        const auto &a = j["a"];
        update.bids.reserve(b.size());
        update.asks.reserve(a.size());

        for (const auto &lvl: b) {
            const auto &px = lvl[0].get_ref<const std::string &>();
            const auto &qt = lvl[1].get_ref<const std::string &>();
            update.bids.push_back(Level{md::parsePriceToTicks(px), md::parseQtyToLots(qt), px, qt});
        }
        for (const auto &lvl: a) {
            const auto &px = lvl[0].get_ref<const std::string &>();
            const auto &qt = lvl[1].get_ref<const std::string &>();
            update.asks.push_back(Level{md::parsePriceToTicks(px), md::parseQtyToLots(qt), px, qt});
        }

        if (debug::dbg_on()) {
            static std::uint64_t inc_cnt = 0;
            if (debug::dbg_sample(inc_cnt)) {
                spdlog::debug("[BINANCE][INC#{}] {}b={} a={} checksum={}",
                    inc_cnt,
                    (md::debug::show_seq.load(std::memory_order_relaxed) ?
                        std::string("prev=") + std::to_string(update.prev_last) + " first=" + std::to_string(update.first_seq) + " last=" + std::to_string(update.last_seq) + " " : ""),
                    update.bids.size(), update.asks.size(),
                    (md::debug::show_checksum.load(std::memory_order_relaxed) ? std::to_string(update.checksum) : ""));
                debug::dbg_raw(msg);
            }
        }

        return true;
    }

    bool BinanceAdapter::parseSnapshot(std::string_view body, GenericSnapshotFormat &snap) const {
        snap.reset();

        try {
            // json j = json::parse(body);
            json j = json::parse(body.begin(), body.end(), nullptr, false);
            if (j.is_discarded()) {
                if (debug::dbg_on()) {
                    spdlog::debug("[BINANCE][SNAPSHOT] json parse failed");
                    debug::dbg_raw(body);
                }
                return false;
            }

            snap.lastUpdateId = j["lastUpdateId"].get<std::uint64_t>();

            for (const auto &b: j["bids"]) {
                const auto &px_str = b[0].get_ref<const std::string &>();
                const auto &qty_str = b[1].get_ref<const std::string &>();
                snap.bids.push_back(Level{parsePriceToTicks(px_str), parseQtyToLots(qty_str), px_str, qty_str});
            }

            for (const auto &a: j["asks"]) {
                const auto &px_str = a[0].get_ref<const std::string &>();
                const auto &qty_str = a[1].get_ref<const std::string &>();
                snap.asks.push_back(Level{parsePriceToTicks(px_str), parseQtyToLots(qty_str), px_str, qty_str});
            }

            if (debug::dbg_on()) {
                static std::uint64_t snap_cnt = 0;
                // snapshot is rare; log always or sample—your choice:
                ++snap_cnt;
                spdlog::debug("[BINANCE][SNAPSHOT#{}] seqId={} bids={} asks={} checksum={}",
                    snap_cnt, snap.lastUpdateId, snap.bids.size(), snap.asks.size(),
                    (md::debug::show_checksum.load(std::memory_order_relaxed) ? std::to_string(snap.checksum) : ""));
                debug::dbg_levels("bid", snap.bids);
                debug::dbg_levels("ask", snap.asks);
                debug::dbg_raw(body);
            }

            return true;
        } catch (...) {
            // log and return std::nullopt
            return false;
        }
    }
}
