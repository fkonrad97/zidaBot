#pragma once

#include <string>
#include <string_view>
#include <cstddef>

#include "abstract/FeedHandler.hpp"
#include "orderbook/OrderBookController.hpp"
#include "utils/CheckSumUtils.hpp"

namespace md {
    enum class SyncMode : std::uint8_t {
        RestAnchored, // REST snapshot can be stitched to WS incremental
        WsAuthoritative // WS snapshot must be the baseline
    };

    struct VenueCaps {
        SyncMode sync_mode{SyncMode::RestAnchored};

        bool ws_sends_snapshot{false}; // expects WS snapshot messages
        bool has_checksum{false}; // checksum available/expected
        bool can_backfill{false}; // can backfill missing ranges
        bool requires_ws_bootstrap{false}; // Kucoin needs bullet-boostrap before connecting to WS
        bool allow_seq_gap{false}; // if true, controller will tolerate non-contiguous sequence numbers

        // resolved checksum policy (cold path)
        ChecksumFn checksum_fn{nullptr};
        std::uint8_t checksum_top_n{25};
    };

    struct EndPoint {
        std::string host;
        std::string port;
        std::string target; /// Can be both WS or REST target
    };

    struct WsBootstrapInfo {
        EndPoint ws; // resolved WS endpoint (host/port/target)
        int ping_interval_ms{0}; // if venue provides it
        int ping_timeout_ms{0}; // if venue provides it
    };

    struct BinanceAdapter {
        /**
         * Cold-path:
         *      - resolve defalts + apply config override if needed
         *      - build subscribe frame
         *      - build REST snapshot target
        */
        VenueCaps caps() const noexcept;

        EndPoint wsEndpoint(const FeedHandlerConfig &cfg) const;

        EndPoint restEndpoint(const FeedHandlerConfig &cfg) const;

        std::string wsSubscribeFrame(const FeedHandlerConfig &cfg) const;

        std::string restSnapshotTarget(const FeedHandlerConfig &cfg) const;

        /**
         * Hot-path:
         *      - cheap filter + parse
         */
        bool isIncremental(std::string_view msg) const noexcept;

        bool parseIncremental(std::string_view msg, GenericIncrementalFormat &out) const;

        /// WS snapshot hooks (for WsAuthoritative venues)
        bool isSnapshot(std::string_view msg) const noexcept { return false; }
        bool parseWsSnapshot(std::string_view msg, GenericSnapshotFormat &) const { return false; }

        /// REST snapshot
        bool parseSnapshot(std::string_view body, GenericSnapshotFormat &out) const;

        // ---- NEW: WS bootstrap hooks (default no-op) ----
        std::string wsBootstrapTarget(const FeedHandlerConfig &) const { return {}; }
        std::string wsBootstrapBody(const FeedHandlerConfig &) const { return {}; }
        bool parseWsBootstrap(std::string_view, std::string_view, WsBootstrapInfo &) const { return false; }
    };

    struct OKXAdapter {
        VenueCaps caps() const noexcept;

        EndPoint wsEndpoint(const FeedHandlerConfig &cfg) const;

        EndPoint restEndpoint(const FeedHandlerConfig &cfg) const;

        std::string wsSubscribeFrame(const FeedHandlerConfig &cfg) const;

        std::string restSnapshotTarget(const FeedHandlerConfig &cfg) const;

        bool isIncremental(std::string_view msg) const noexcept;

        bool parseIncremental(std::string_view msg, GenericIncrementalFormat &out) const;

        bool isSnapshot(std::string_view msg) const noexcept;

        bool parseWsSnapshot(std::string_view msg, GenericSnapshotFormat &out) const;

        /// OKX does not use REST Snapshots, so can return false
        bool parseSnapshot(std::string_view, GenericSnapshotFormat &) const { return false; }

        // ---- NEW: WS bootstrap hooks (default no-op) ----
        std::string wsBootstrapTarget(const FeedHandlerConfig &) const { return {}; }
        std::string wsBootstrapBody(const FeedHandlerConfig &) const { return {}; }
        bool parseWsBootstrap(std::string_view, std::string_view, WsBootstrapInfo &) const { return false; }
    };

    /// https://www.bitget.com/api-doc/spot/websocket/public/Depth-Channel
    struct BitgetAdapter {
        VenueCaps caps() const noexcept;

        EndPoint wsEndpoint(const FeedHandlerConfig &cfg) const;

        EndPoint restEndpoint(const FeedHandlerConfig &cfg) const;

        std::string wsSubscribeFrame(const FeedHandlerConfig &cfg) const;

        std::string restSnapshotTarget(const FeedHandlerConfig &cfg) const;

        bool isIncremental(std::string_view msg) const noexcept;

        bool parseIncremental(std::string_view msg, GenericIncrementalFormat &out) const;

        bool isSnapshot(std::string_view msg) const noexcept;

        bool parseWsSnapshot(std::string_view msg, GenericSnapshotFormat &out) const;

        bool parseSnapshot(std::string_view, GenericSnapshotFormat &) const { return false; };

        // ---- NEW: WS bootstrap hooks (default no-op) ----
        std::string wsBootstrapTarget(const FeedHandlerConfig &) const { return {}; }
        std::string wsBootstrapBody(const FeedHandlerConfig &) const { return {}; }
        bool parseWsBootstrap(std::string_view, std::string_view, WsBootstrapInfo &) const { return false; }
    };

    struct BybitAdapter {
        VenueCaps caps() const noexcept;

        EndPoint wsEndpoint(const FeedHandlerConfig &cfg) const;

        EndPoint restEndpoint(const FeedHandlerConfig &cfg) const;

        std::string wsSubscribeFrame(const FeedHandlerConfig &cfg) const;

        std::string restSnapshotTarget(const FeedHandlerConfig &cfg) const;

        bool isIncremental(std::string_view msg) const noexcept;

        bool parseIncremental(std::string_view msg, GenericIncrementalFormat &out) const;

        bool isSnapshot(std::string_view msg) const noexcept;

        bool parseWsSnapshot(std::string_view msg, GenericSnapshotFormat &out) const;

        bool parseSnapshot(std::string_view, GenericSnapshotFormat &) const { return false; };

        // ---- NEW: WS bootstrap hooks (default no-op) ----
        std::string wsBootstrapTarget(const FeedHandlerConfig &) const { return {}; }
        std::string wsBootstrapBody(const FeedHandlerConfig &) const { return {}; }
        bool parseWsBootstrap(std::string_view, std::string_view, WsBootstrapInfo &) const { return false; }
    };

    struct KucoinAdapter {
        VenueCaps caps() const noexcept;

        EndPoint wsEndpoint(const FeedHandlerConfig &cfg) const;

        EndPoint restEndpoint(const FeedHandlerConfig &cfg) const;

        std::string wsSubscribeFrame(const FeedHandlerConfig &cfg) const;

        std::string restSnapshotTarget(const FeedHandlerConfig &cfg) const;

        bool isIncremental(std::string_view msg) const noexcept;

        bool parseIncremental(std::string_view msg, GenericIncrementalFormat &out) const;

        bool isSnapshot(std::string_view) const noexcept { return false; };

        bool parseWsSnapshot(std::string_view msg, GenericSnapshotFormat &) const { return false; };

        bool parseSnapshot(std::string_view, GenericSnapshotFormat &) const;

        // ---- NEW: KuCoin uses WS bootstrap (bullet-public) ----
        std::string wsBootstrapTarget(const FeedHandlerConfig &) const;

        std::string wsBootstrapBody(const FeedHandlerConfig &) const { return {}; } // empty POST
        bool parseWsBootstrap(std::string_view body, std::string_view connect_id, WsBootstrapInfo &out) const;
    };
}
