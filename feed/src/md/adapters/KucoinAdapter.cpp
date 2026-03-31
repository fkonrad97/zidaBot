#include <nlohmann/json.hpp>

#include "utils/VenueUtils.hpp"
#include "md/VenueAdapter.hpp"
#include "orderbook/OrderBookUtils.hpp"
#include "utils/DebugConfigUtils.hpp"
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace md {
    static bool is_num_or_str(const json &j) noexcept {
        return j.is_number_integer() || j.is_number_unsigned() || j.is_string();
    }

    static bool as_u64(const json &j, std::uint64_t &out) noexcept {
        try {
            if (j.is_string()) {
                const std::string s = j.get<std::string>();
                if (s.empty() || s[0] == '-') return false;
                std::size_t pos = 0;
                const auto v = std::stoull(s, &pos);
                if (pos != s.size()) return false;
                out = v;
                return true;
            }
            if (j.is_number_unsigned()) {
                out = j.get<std::uint64_t>();
                return true;
            }
            if (j.is_number_integer()) {
                const auto v = j.get<std::int64_t>();
                if (v < 0) return false;
                out = static_cast<std::uint64_t>(v);
                return true;
            }
            return false;
        } catch (...) {
            return false;
        }
    }

    static void parseLevels2col(const json &arr, std::vector<Level> &out) {
        // KuCoin REST bids/asks typically: [["price","size"], ...]
        // KuCoin WS changes bids/asks typically: [["price","size","seq"], ...]
        out.clear();
        if (!arr.is_array()) return;

        out.reserve(arr.size());
        for (const auto &lvl: arr) {
            if (!lvl.is_array() || lvl.size() < 2) continue;

            // price/size are strings in docs; be defensive
            std::string px, sz;
            if (lvl[0].is_string()) px = lvl[0].get<std::string>();
            else px = lvl[0].dump();

            if (lvl[1].is_string()) sz = lvl[1].get<std::string>();
            else sz = lvl[1].dump();

            out.push_back(Level{
                md::parsePriceToTicks(px),
                md::parseQtyToLots(sz),
                px,
                sz
            });
        }
    }

    static bool parse_wss_endpoint(std::string_view endpoint,
                                   std::string &out_host,
                                   std::string &out_port,
                                   std::string &out_path) {
        // endpoint example: "wss://ws-api-spot.kucoin.com/endpoint"
        // or "wss://ws-api-spot.kucoin.com:443/endpoint"
        constexpr std::string_view prefix = "wss://";
        if (!endpoint.starts_with(prefix)) return false;

        endpoint.remove_prefix(prefix.size());

        const std::size_t slash = endpoint.find('/');
        const std::string_view hostport = (slash == std::string_view::npos) ? endpoint : endpoint.substr(0, slash);
        out_path = (slash == std::string_view::npos) ? "/" : std::string(endpoint.substr(slash));

        const std::size_t colon = hostport.find(':');
        if (colon == std::string_view::npos) {
            out_host = std::string(hostport);
            out_port = "443";
        } else {
            out_host = std::string(hostport.substr(0, colon));
            out_port = std::string(hostport.substr(colon + 1));
            if (out_port.empty()) out_port = "443";
        }
        return !out_host.empty();
    }

    std::string KucoinAdapter::wsBootstrapTarget(const FeedHandlerConfig &) const {
        // KuCoin public WS bootstrap
        return "/api/v1/bullet-public";
    }

    bool KucoinAdapter::parseWsBootstrap(std::string_view body,
                                         std::string_view connect_id,
                                         WsBootstrapInfo &out) const {
        out = WsBootstrapInfo{};

        json j = json::parse(body.begin(), body.end(), nullptr, false);
        if (j.is_discarded()) {
            if (debug::dbg_on()) {
                spdlog::debug("[KUCOIN][BOOTSTRAP] json parse failed");
                debug::dbg_raw(body);
            }
            return false;
        }

        if (!j.contains("data")) return false;
        const auto &d = j["data"];

        if (!d.contains("token") || !d["token"].is_string()) return false;
        const std::string token = d["token"].get<std::string>();

        if (!d.contains("instanceServers") || !d["instanceServers"].is_array() || d["instanceServers"].empty()) {
            return false;
        }

        const auto &s0 = d["instanceServers"][0];

        // endpoint is something like "wss://ws-api-spot.kucoin.com/endpoint"
        if (!s0.contains("endpoint") || !s0["endpoint"].is_string()) return false;
        const std::string endpoint = s0["endpoint"].get<std::string>();

        int pingInterval = 0;
        int pingTimeout = 0;
        if (s0.contains("pingInterval") && (s0["pingInterval"].is_number_integer() || s0["pingInterval"].
                                            is_number_unsigned())) {
            pingInterval = s0["pingInterval"].get<int>();
        }
        if (s0.contains("pingTimeout") && (s0["pingTimeout"].is_number_integer() || s0["pingTimeout"].
                                           is_number_unsigned())) {
            pingTimeout = s0["pingTimeout"].get<int>();
        }

        std::string host, port, path;
        if (!parse_wss_endpoint(endpoint, host, port, path)) return false;

        // Final WS target: "<path>?token=...&connectId=..."
        std::string target = path;
        if (target.find('?') == std::string::npos) target += "?";
        else target += "&";
        target += "token=" + token + "&connectId=" + std::string(connect_id);

        out.ws.host = std::move(host);
        out.ws.port = std::move(port);
        out.ws.target = std::move(target);
        out.ping_interval_ms = pingInterval;
        out.ping_timeout_ms = pingTimeout;

        if (debug::dbg_on()) {
            spdlog::debug("[KUCOIN][BOOTSTRAP] host={} port={} target={} pingInterval={} pingTimeout={}",
                out.ws.host, out.ws.port, out.ws.target,
                out.ping_interval_ms, out.ping_timeout_ms);
        }

        return true;
    }

    VenueCaps KucoinAdapter::caps() const noexcept {
        VenueCaps c;
        c.sync_mode = SyncMode::RestAnchored;
        c.ws_sends_snapshot = false;
        c.has_checksum = false;
        c.can_backfill = false;
        c.requires_ws_bootstrap = true;
        // KuCoin partial snapshots may leave gaps between the snapshot's
        // sequence and the first incremental update we receive.  Allow the
        // controller to jump ahead rather than constantly resyncing.
        c.allow_seq_gap = true;
        return c;
    }

    EndPoint KucoinAdapter::wsEndpoint(const FeedHandlerConfig &cfg) const {
        EndPoint e;
        e.host = cfg.ws_host.empty() ? "ws-api-spot.kucoin.com" : cfg.ws_host;
        e.port = cfg.ws_port.empty() ? "443" : cfg.ws_port;

        // IMPORTANT:
        // KuCoin WS requires a bullet token to connect:
        // wss://ws-api-spot.kucoin.com/?token=xxx&connectId=...
        // So cfg.ws_path should ideally already contain "/?token=...&connectId=..."
        // until you implement the bullet-public bootstrap.
        e.target = cfg.ws_path.empty() ? "/v5/public/spot" : cfg.ws_path;

        return e;
    }

    EndPoint KucoinAdapter::restEndpoint(const FeedHandlerConfig &cfg) const {
        EndPoint e;
        e.host = cfg.rest_host.empty() ? "api.kucoin.com" : cfg.rest_host;
        e.port = cfg.rest_port.empty() ? "443" : cfg.rest_port;
        e.target = cfg.rest_path;
        return e;
    }

    std::string KucoinAdapter::wsSubscribeFrame(const FeedHandlerConfig &cfg) const {
        // KuCoin symbol for WS L2 is dashed: "BTC-USDT"
        const std::string sym = venue::map_ws_symbol(VenueId::KUCOIN, cfg.base_ccy, cfg.quote_ccy);

        json j;
        // id is an arbitrary client-generated identifier; keep it simple
        j["id"] = "1";
        j["type"] = "subscribe";
        j["topic"] = "/market/level2:" + sym;
        j["privateChannel"] = false;
        j["response"] = true;
        return j.dump();
    }

    std::string KucoinAdapter::restSnapshotTarget(const FeedHandlerConfig &cfg) const {
        const std::string sym = venue::map_rest_symbol(VenueId::KUCOIN, cfg.base_ccy, cfg.quote_ccy);

        // KuCoin public REST snapshot is "part orderbook" with a size suffix.
        // Example from docs: /api/v1/market/orderbook/level2_20?symbol=BTC-USDT
        // If you want 200, KuCoin does NOT offer a public 200 snapshot; cap at 100 (or pick 20/50/100 policy).
        const int requested = static_cast<int>(cfg.depthLevel);
        const int size = (requested <= 20) ? 20 : 100; // practical policy; cap to 100 for public REST

        return "/api/v1/market/orderbook/level2_" + std::to_string(size) + "?symbol=" + sym;
    }

    bool KucoinAdapter::isIncremental(std::string_view msg) const noexcept {
        // cheap prefilter
        // Example message: {"type":"message","topic":"/market/level2:BTC-USDT","subject":"trade.l2update","data":{...}}
        if (msg.find("\"type\":\"message\"") == std::string_view::npos) return false;
        if (msg.find("\"subject\":\"trade.l2update\"") == std::string_view::npos) return false;
        if (msg.find("\"/market/level2:") == std::string_view::npos) return false;
        return true;
    }

    bool KucoinAdapter::parseSnapshot(std::string_view msg, GenericSnapshotFormat &out) const {
        out.reset();

        json j = json::parse(msg.begin(), msg.end(), nullptr, false);
        if (j.is_discarded()) {
            if (debug::dbg_on()) {
                spdlog::debug("[KUCOIN][SNAPSHOT] json parse failed");
                debug::dbg_raw(msg);
            }
            return false;
        }

        if (!j.contains("data")) return false;
        const auto &d = j["data"];

        // data.sequence can be string (commonly) — be defensive
        if (!d.contains("sequence") || !is_num_or_str(d["sequence"])) return false;
        if (!as_u64(d["sequence"], out.lastUpdateId)) return false;

        if (d.contains("bids")) parseLevels2col(d["bids"], out.bids);
        if (d.contains("asks")) parseLevels2col(d["asks"], out.asks);

        if (debug::dbg_on()) {
            static std::uint64_t snap_cnt = 0;
            ++snap_cnt;
            spdlog::debug("[KUCOIN][SNAPSHOT#{}] seqId={} bids={} asks={} checksum={}",
                snap_cnt, out.lastUpdateId, out.bids.size(), out.asks.size(),
                (md::debug::show_checksum.load(std::memory_order_relaxed) ? std::to_string(out.checksum) : ""));
            debug::dbg_levels("bid", out.bids);
            debug::dbg_levels("ask", out.asks);
            debug::dbg_raw(msg);
        }

        return true;
    }

    bool KucoinAdapter::parseIncremental(std::string_view msg, GenericIncrementalFormat &out) const {
        out.reset();

        json j = json::parse(msg.begin(), msg.end(), nullptr, false);
        if (j.is_discarded()) {
            if (debug::dbg_on()) {
                spdlog::debug("[KUCOIN][INC] json parse failed");
                debug::dbg_raw(msg);
            }
            return false;
        }

        if (j.value("type", "") != "message") return false;
        if (j.value("subject", "") != "trade.l2update") return false;
        if (!j.contains("data")) return false;

        const auto &d = j["data"];

        if (!d.contains("sequenceStart") || !d.contains("sequenceEnd")) return false;
        if (!is_num_or_str(d["sequenceStart"]) || !is_num_or_str(d["sequenceEnd"])) return false;

        if (!as_u64(d["sequenceStart"], out.first_seq)) return false;
        if (!as_u64(d["sequenceEnd"], out.last_seq)) return false;
        out.prev_last = (out.first_seq > 0) ? (out.first_seq - 1) : 0;

        if (!d.contains("changes")) return false;
        const auto &ch = d["changes"];

        if (ch.contains("bids")) parseLevels2col(ch["bids"], out.bids);
        if (ch.contains("asks")) parseLevels2col(ch["asks"], out.asks);

        if (debug::dbg_on()) {
            static std::uint64_t inc_cnt = 0;
            if (debug::dbg_sample(inc_cnt)) {
                spdlog::debug("[KUCOIN][INC#{}] {}b={} a={} checksum={}",
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
}
