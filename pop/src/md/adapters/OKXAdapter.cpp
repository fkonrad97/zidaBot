#include <nlohmann/json.hpp>

#include "utils/VenueUtils.hpp"
#include "md/VenueAdapter.hpp"
#include "orderbook/OrderBookUtils.hpp"
#include "utils/DebugConfigUtils.hpp"
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace md {
    VenueCaps OKXAdapter::caps() const noexcept {
        VenueCaps c;
        c.sync_mode = SyncMode::WsAuthoritative;
        c.ws_sends_snapshot = true;
        // TODO: Revisit OKX checksum/snapshot assumptions when exchange-side
        // deprecation rollout completes; prefer seq-based integrity if checksum
        // is removed from the feed.
        c.has_checksum = true;
        c.can_backfill = false;
        return c;
    }

    EndPoint OKXAdapter::wsEndpoint(const FeedHandlerConfig &cfg) const {
        EndPoint e;
        e.host = cfg.ws_host.empty() ? "wseea.okx.com" : cfg.ws_host;
        e.port = cfg.ws_port.empty() ? "8443" : cfg.ws_port;
        e.target = cfg.ws_path.empty() ? "/ws/v5/public" : cfg.ws_path;
        return e;
    }

    EndPoint OKXAdapter::restEndpoint(const FeedHandlerConfig &cfg) const {
        EndPoint e;
        e.host = cfg.rest_host.empty() ? "eea.okx.com" : cfg.rest_host;
        e.port = cfg.rest_port.empty() ? "443" : cfg.rest_port;
        e.target = cfg.rest_path; // usually empty; per-request target returned by restSnapshotTarget()
        return e;
    }

    std::string OKXAdapter::wsSubscribeFrame(const FeedHandlerConfig &cfg) const {
        const std::string instId = venue::map_ws_symbol(VenueId::OKX, cfg.base_ccy, cfg.quote_ccy); // "BTC-USDT"

        json j;
        j["op"] = "subscribe";
        j["args"] = json::array({{{"channel", "books"}, {"instId", instId}}});
        return j.dump();
    }

    std::string OKXAdapter::restSnapshotTarget(const FeedHandlerConfig &cfg) const {
        // Optional for WS-authoritative mode (useful for debugging / fallback)
        const std::string instId = venue::map_rest_symbol(VenueId::OKX, cfg.base_ccy, cfg.quote_ccy);
        // likely "BTC-USDT"

        // GET /api/v5/market/books?instId=BTC-USDT&sz=400
        const std::size_t sz = std::min<std::size_t>(cfg.depthLevel, 400);
        return "/api/v5/market/books?instId=" + instId + "&sz=" + std::to_string(sz);
    }

    static bool looksLikeOKXBooks(std::string_view msg) noexcept {
        // cheap prefilter (replace with simdjson later)
        return msg.find("\"channel\":\"books") != std::string_view::npos
               && msg.find("\"data\"") != std::string_view::npos;
    }

    bool OKXAdapter::isSnapshot(std::string_view msg) const noexcept {
        if (!looksLikeOKXBooks(msg)) return false;
        return msg.find("\"action\":\"snapshot\"") != std::string_view::npos;
    }

    bool OKXAdapter::isIncremental(std::string_view msg) const noexcept {
        if (!looksLikeOKXBooks(msg)) return false;
        return msg.find("\"action\":\"update\"") != std::string_view::npos;
    }

    static void parseLevels2col(const json &arr, std::vector<Level> &out) {
        // OKX asks/bids entries look like ["8476.98","415","0","13"]
        out.clear();
        if (!arr.is_array()) return;
        out.reserve(arr.size());
        for (const auto &lvl: arr) {
            if (!lvl.is_array() || lvl.size() < 2) continue;
            const auto &px = lvl[0].get_ref<const std::string &>();
            const auto &sz = lvl[1].get_ref<const std::string &>();
            out.push_back(Level{md::parsePriceToTicks(px), md::parseQtyToLots(sz), px, sz});
        }
    }

    bool OKXAdapter::parseWsSnapshot(std::string_view msg, GenericSnapshotFormat &out) const {
        out.reset();

        json j = json::parse(msg.begin(), msg.end(), nullptr, false);
        if (j.is_discarded()) {
            if (debug::dbg_on()) {
                spdlog::debug("[OKX][SNAPSHOT] json parse failed");
                debug::dbg_raw(msg);
            }
            return false;
        }

        if (j.value("action", "") != "snapshot") return false;
        if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) return false;

        const auto &d0 = j["data"][0];

        if (!d0.contains("seqId")) return false;
        out.lastUpdateId = d0["seqId"].get<std::uint64_t>();

        // NEW: checksum (prefer data[0], fallback to top-level if ever present)
        if (d0.contains("checksum")) {
            // checksum sometimes comes as number; if string, handle below
            if (d0["checksum"].is_string()) out.checksum = std::stoll(d0["checksum"].get<std::string>());
            else out.checksum = d0["checksum"].get<std::int64_t>();
        } else if (j.contains("checksum")) {
            if (j["checksum"].is_string()) out.checksum = std::stoll(j["checksum"].get<std::string>());
            else out.checksum = j["checksum"].get<std::int64_t>();
        }

        if (d0.contains("bids")) parseLevels2col(d0["bids"], out.bids);
        if (d0.contains("asks")) parseLevels2col(d0["asks"], out.asks);

        if (debug::dbg_on()) {
            static std::uint64_t snap_cnt = 0;
            // snapshot is rare; log always or sample—your choice:
            ++snap_cnt;
            spdlog::debug("[OKX][SNAPSHOT#{}] seqId={} bids={} asks={} checksum={}",
                snap_cnt, out.lastUpdateId, out.bids.size(), out.asks.size(),
                (md::debug::show_checksum.load(std::memory_order_relaxed) ? std::to_string(out.checksum) : ""));
            debug::dbg_levels("bid", out.bids);
            debug::dbg_levels("ask", out.asks);
            debug::dbg_raw(msg);
        }

        return true;
    }

    bool OKXAdapter::parseIncremental(std::string_view msg, GenericIncrementalFormat &out) const {
        out.reset();

        json j = json::parse(msg.begin(), msg.end(), nullptr, false);
        if (j.is_discarded()) {
            if (debug::dbg_on()) {
                spdlog::debug("[OKX][INC] json parse failed");
                debug::dbg_raw(msg);
            }
            return false;
        }

        if (j.value("action", "") != "update") return false;
        if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) return false;

        const auto &d0 = j["data"][0];

        if (!d0.contains("seqId") || !d0.contains("prevSeqId")) return false;

        const std::int64_t prev = d0["prevSeqId"].get<std::int64_t>();
        const std::uint64_t seq = d0["seqId"].get<std::uint64_t>();

        out.prev_last = (prev < 0) ? 0ULL : static_cast<std::uint64_t>(prev);
        out.last_seq = seq;
        out.first_seq = out.prev_last + 1;

        // NEW: checksum
        if (d0.contains("checksum")) {
            if (d0["checksum"].is_string()) out.checksum = std::stoll(d0["checksum"].get<std::string>());
            else out.checksum = d0["checksum"].get<std::int64_t>();
        } else if (j.contains("checksum")) {
            if (j["checksum"].is_string()) out.checksum = std::stoll(j["checksum"].get<std::string>());
            else out.checksum = j["checksum"].get<std::int64_t>();
        }

        if (d0.contains("bids")) parseLevels2col(d0["bids"], out.bids);
        if (d0.contains("asks")) parseLevels2col(d0["asks"], out.asks);

        if (debug::dbg_on()) {
            static std::uint64_t inc_cnt = 0;
            if (debug::dbg_sample(inc_cnt)) {
                spdlog::debug("[OKX][INC#{}] {}b={} a={} checksum={}",
                    inc_cnt,
                    (md::debug::show_seq.load(std::memory_order_relaxed) ?
                        std::string("prev=") + std::to_string(out.prev_last) + " first=" + std::to_string(out.first_seq) + " last=" + std::to_string(out.last_seq) + " " : ""),
                    out.bids.size(), out.asks.size(),
                    (md::debug::show_checksum.load(std::memory_order_relaxed) ? std::to_string(out.checksum) : ""));
                debug::dbg_raw(msg);
            }
        }

        return true;
    }
} // namespace md
