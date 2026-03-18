#include <nlohmann/json.hpp>

#include "utils/VenueUtils.hpp"
#include "md/VenueAdapter.hpp"
#include "orderbook/OrderBookUtils.hpp"
#include "utils/DebugConfigUtils.hpp"

using json = nlohmann::json;

/// https://bybit-exchange.github.io/docs/v5/websocket/public/orderbook
/**
* TODO - Bybit orderbook stream does not support any depth - have to constrain it
*/
namespace md {
    VenueCaps BybitAdapter::caps() const noexcept {
        VenueCaps c;
        c.sync_mode = SyncMode::WsAuthoritative;
        c.ws_sends_snapshot = true;
        c.has_checksum = false;
        c.can_backfill = false;
        return c;
    }

    EndPoint BybitAdapter::wsEndpoint(const FeedHandlerConfig &cfg) const {
        EndPoint e;
        e.host = cfg.ws_host.empty() ? "stream.bybit.com" : cfg.ws_host;
        e.port = cfg.ws_port.empty() ? "443" : cfg.ws_port;
        e.target = cfg.ws_path.empty() ? "/v5/public/spot" : cfg.ws_path;
        return e;
    }

    EndPoint BybitAdapter::restEndpoint(const FeedHandlerConfig &cfg) const {
        EndPoint e;
        e.host = cfg.rest_host.empty() ? "api.bybit.com" : cfg.rest_host;
        e.port = cfg.rest_port.empty() ? "443" : cfg.rest_port;
        e.target = cfg.rest_path; // usually empty; per-request target returned by restSnapshotTarget()
        return e;
    }

    std::string BybitAdapter::wsSubscribeFrame(const FeedHandlerConfig &cfg) const {
        const std::string instId = venue::map_ws_symbol(VenueId::BYBIT, cfg.base_ccy, cfg.quote_ccy);

        // Bybit spot WS orderbook only supports depths 1, 50, 200.
        // Snap the requested depth to the largest valid value that does not exceed it.
        // Depths above 200 are silently ignored by Bybit (no data arrives), so we must cap.
        std::size_t depth = cfg.depthLevel;
        if (depth >= 200) depth = 200;
        else if (depth >= 50) depth = 50;
        else depth = 1;

        json j;
        j["op"] = "subscribe";
        j["args"] = json::array({"orderbook." + std::to_string(depth) + "." + instId});
        return j.dump();
    }

    std::string BybitAdapter::restSnapshotTarget(const FeedHandlerConfig &) const {
        return "";
    }

    bool BybitAdapter::isSnapshot(std::string_view msg) const noexcept {
        return msg.find("\"type\":\"snapshot\"") != std::string_view::npos;
    }

    bool BybitAdapter::isIncremental(std::string_view msg) const noexcept {
        return msg.find("\"type\":\"delta\"") != std::string_view::npos;
    }

    static void parseLevels2col_bybit(const json &arr, std::vector<Level> &out) {
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

    bool BybitAdapter::parseWsSnapshot(std::string_view msg, GenericSnapshotFormat &out) const {
        out.reset();

        json j = json::parse(msg.begin(), msg.end(), nullptr, false);
        if (j.is_discarded()) {
            if (debug::dbg_on()) {
                std::cerr << "[BYBIT][SNAPSHOT] json parse failed\n";
                debug::dbg_raw(msg);
            }
            return false;
        }

        if (j.value("type", "") != "snapshot") return false;
        if (!j.contains("data") || !j["data"].is_object()) return false;

        const auto &d = j["data"];

        // Bybit orderbook update id
        if (!d.contains("u")) return false;
        out.lastUpdateId = d["u"].get<std::uint64_t>();

        // No checksum in Bybit v5 orderbook
        out.checksum = 0;

        if (d.contains("b")) parseLevels2col_bybit(d["b"], out.bids);
        if (d.contains("a")) parseLevels2col_bybit(d["a"], out.asks);

        if (debug::dbg_on()) {
            static std::uint64_t snap_cnt = 0;
            ++snap_cnt;
            std::cerr << "[BYBIT][SNAPSHOT#" << snap_cnt << "] "
                    << "seqId=" << out.lastUpdateId
                    << " bids=" << out.bids.size()
                    << " asks=" << out.asks.size();
            if (md::debug::show_checksum.load(std::memory_order_relaxed)) {
                std::cerr << " checksum=" << out.checksum;
            }
            std::cerr << "\n";
            debug::dbg_levels("bid", out.bids);
            debug::dbg_levels("ask", out.asks);
            debug::dbg_raw(msg);
        }

        return true;
    }

    bool BybitAdapter::parseIncremental(std::string_view msg, GenericIncrementalFormat &out) const {
        out.reset();

        json j = json::parse(msg.begin(), msg.end(), nullptr, false);
        if (j.is_discarded()) {
            if (debug::dbg_on()) {
                std::cerr << "[BYBIT][INC] json parse failed\n";
                debug::dbg_raw(msg);
            }
            return false;
        }

        if (j.value("type", "") != "delta") return false;
        if (!j.contains("data") || !j["data"].is_object()) return false;

        const auto &d = j["data"];

        if (!d.contains("u")) return false;
        const std::uint64_t u = d["u"].get<std::uint64_t>();

        // Map to your generic seq fields as a single-id update:
        out.first_seq = u;
        out.last_seq = u;
        out.prev_last = (u > 0) ? (u - 1) : 0;
        out.checksum = 0;

        if (d.contains("b")) parseLevels2col_bybit(d["b"], out.bids);
        if (d.contains("a")) parseLevels2col_bybit(d["a"], out.asks);

        if (debug::dbg_on()) {
            static std::uint64_t inc_cnt = 0;
            if (debug::dbg_sample(inc_cnt)) {
                std::cerr << "[BYBIT][INC#" << inc_cnt << "] ";
                if (md::debug::show_seq.load(std::memory_order_relaxed)) {
                    std::cerr << "prev=" << out.prev_last
                            << " first=" << out.first_seq
                            << " last=" << out.last_seq << " ";
                }
                std::cerr << "b=" << out.bids.size()
                        << " a=" << out.asks.size();
                if (md::debug::show_checksum.load(std::memory_order_relaxed)) {
                    std::cerr << " checksum=" << out.checksum;
                }
                std::cerr << "\n";
                debug::dbg_raw(msg);
            }
        }

        return true;
    }
}
