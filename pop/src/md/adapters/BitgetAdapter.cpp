#include <nlohmann/json.hpp>

#include "utils/VenueUtils.hpp"
#include "md/VenueAdapter.hpp"
#include "orderbook/OrderBookUtils.hpp"
#include "utils/DebugConfigUtils.hpp"
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace md {
    VenueCaps BitgetAdapter::caps() const noexcept {
        VenueCaps c;
        c.sync_mode = SyncMode::WsAuthoritative;
        c.ws_sends_snapshot = true;

        c.has_checksum = false;
        // Bitget CRC32 checksum validation is disabled for the following reasons:
        //
        //   1. The initial WS snapshot rarely includes a checksum, so the baseline
        //      is accepted as best-effort (no CRC32 to anchor against).
        //
        //   2. The snapshot's seq can be behind the live stream by hundreds of
        //      incremental updates (snapshot prepared at subscribe-time, stream has
        //      advanced). Even with allow_seq_gap=true, the book state after
        //      applying the snapshot may differ from Bitget's expected state,
        //      causing incremental CRC32 validation to fail spuriously.
        //
        //   3. Structural integrity is ensured by C1 (crossed-book guard),
        //      B3 (tick/quantity sanity), and C3 (periodic OrderBook::validate()).
        //
        // Re-enable when a REST snapshot is used as the verified baseline.
        c.checksum_fn = nullptr;
        c.checksum_top_n = 25;

        c.can_backfill = false;
        c.allow_seq_gap = true;
        return c;
    }

    EndPoint BitgetAdapter::wsEndpoint(const FeedHandlerConfig &cfg) const {
        EndPoint e;
        e.host = cfg.ws_host.empty() ? "ws.bitget.com" : cfg.ws_host;
        e.port = cfg.ws_port.empty() ? "443" : cfg.ws_port;
        e.target = cfg.ws_path.empty() ? "/v2/ws/public" : cfg.ws_path;
        return e;
    }

    EndPoint BitgetAdapter::restEndpoint(const FeedHandlerConfig &cfg) const {
        EndPoint e;
        e.host = cfg.rest_host.empty() ? "api.bitget.com" : cfg.rest_host;
        e.port = cfg.rest_port.empty() ? "443" : cfg.rest_port;
        e.target = cfg.rest_path;
        return e;
    }

    std::string BitgetAdapter::wsSubscribeFrame(const FeedHandlerConfig &cfg) const {
        const std::string instId = venue::map_ws_symbol(VenueId::BITGET, cfg.base_ccy, cfg.quote_ccy);

        json j;
        j["op"] = "subscribe";
        j["args"] = json::array({
            {
                {"instType", "SPOT"},
                {"channel", "books"},
                {"instId", instId}
            }
        });
        return j.dump();
    }

    std::string BitgetAdapter::restSnapshotTarget(const FeedHandlerConfig &cfg) const {
        return "";
    }

    static bool looksLikeBitgetBooks(std::string_view msg) noexcept {
        return msg.find("\"channel\":\"books") != std::string_view::npos
               && msg.find("\"data\"") != std::string_view::npos;
    }

    bool BitgetAdapter::isSnapshot(std::string_view msg) const noexcept {
        if (!looksLikeBitgetBooks(msg))
            return false;
        return msg.find("\"action\":\"snapshot\"") != std::string_view::npos;
    }

    bool BitgetAdapter::isIncremental(std::string_view msg) const noexcept {
        if (!looksLikeBitgetBooks(msg)) return false;
        return msg.find("\"action\":\"update\"") != std::string_view::npos;
    }

    static void parseLevels2col(const json &arr, std::vector<Level> &out) {
        out.clear();
        if (!arr.is_array()) return;
        out.reserve(arr.size());
        for (const auto &lvl: arr) {
            if (!lvl.is_array() || lvl.size() < 2) continue;
            const auto &px = lvl[0].get_ref<const std::string &>();
            const auto &qt = lvl[1].get_ref<const std::string &>();
            out.push_back(Level{parsePriceToTicks(px), parseQtyToLots(qt), px, qt});
        }
    }

    bool BitgetAdapter::parseWsSnapshot(std::string_view msg, GenericSnapshotFormat &out) const {
        out.reset();

        json j = json::parse(msg.begin(), msg.end(), nullptr, false);
        if (j.is_discarded()) {
            if (debug::dbg_on()) {
                spdlog::debug("[BITGET][SNAPSHOT] json parse failed");
                debug::dbg_raw(msg);
            }
            return false;
        }

        if (j.value("action", "") != "snapshot") return false;
        if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) return false;

        const auto &d0 = j["data"][0];
        if (!d0.contains("bids") || !d0.contains("asks")) return false;

        // seq (Long) -> lastUpdateId anchor (preferred over ts)
        if (d0.contains("seq")) {
            std::uint64_t seq_u64{};
            if (json_to_u64_flexible(d0["seq"], seq_u64)) {
                out.lastUpdateId = seq_u64;
            }
        }

        // checksum
        if (d0.contains("checksum")) {
            if (d0["checksum"].is_string()) out.checksum = std::stoll(d0["checksum"].get<std::string>());
            else out.checksum = d0["checksum"].get<std::int64_t>();
        } else if (j.contains("checksum")) {
            if (j["checksum"].is_string()) out.checksum = std::stoll(j["checksum"].get<std::string>());
            else out.checksum = j["checksum"].get<std::int64_t>();
        }

        parseLevels2col(d0["bids"], out.bids);
        parseLevels2col(d0["asks"], out.asks);

        if (debug::dbg_on()) {
            static std::uint64_t snap_cnt = 0;
            // snapshot is rare; log always or sample—your choice:
            ++snap_cnt;
            spdlog::debug("[BITGET][SNAPSHOT#{}] seqId={} bids={} asks={} checksum={}",
                snap_cnt, out.lastUpdateId, out.bids.size(), out.asks.size(),
                (md::debug::show_checksum.load(std::memory_order_relaxed) ? std::to_string(out.checksum) : ""));
            debug::dbg_levels("bid", out.bids);
            debug::dbg_levels("ask", out.asks);
            debug::dbg_raw(msg);
        }

        return true;
    }

    bool BitgetAdapter::parseIncremental(std::string_view msg, GenericIncrementalFormat &out) const {
        out.reset();

        json j = json::parse(msg.begin(), msg.end(), nullptr, false);
        if (j.is_discarded()) {
            if (debug::dbg_on()) {
                spdlog::debug("[BITGET][INC] json parse failed");
                debug::dbg_raw(msg);
            }
            return false;
        }

        if (j.value("action", "") != "update") return false;
        if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) return false;

        const auto &d0 = j["data"][0];
        if (!d0.contains("bids") || !d0.contains("asks")) return false;

        // seq (Long): treat as single-step sequence
        if (d0.contains("seq")) {
            std::uint64_t seq_u64{};
            if (json_to_u64_flexible(d0["seq"], seq_u64)) {
                out.first_seq = seq_u64;
                out.last_seq = seq_u64;
                out.prev_last = (seq_u64 > 0) ? (seq_u64 - 1) : 0;
            }
        }

        // checksum (Long)
        if (d0.contains("checksum")) {
            std::int64_t c{};
            if (json_to_i64_flexible(d0["checksum"], c)) {
                out.checksum = c;
            }
        }

        parseLevels2col(d0["bids"], out.bids);
        parseLevels2col(d0["asks"], out.asks);

        if (debug::dbg_on()) {
            static std::uint64_t inc_cnt = 0;
            if (debug::dbg_sample(inc_cnt)) {
                spdlog::debug("[BITGET][INC#{}] {}b={} a={} checksum={}",
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
