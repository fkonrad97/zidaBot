#pragma once

#include <boost/asio/io_context.hpp>  /// External event loop
#include <string>

namespace md {
    /**
     * @brief Result of a feed lifecycle API call.
     *
     * This is intentionally limited to immediate call success/failure.
     * It does not describe the live runtime state of the feed after start.
     */
    enum class FeedOpResult {
        OK,
        ERROR
    };

    enum class VenueId { BINANCE, OKX, BYBIT, BITGET, KUCOIN, UNKNOWN };

    inline const char *to_string(VenueId k) {
        switch (k) {
            case VenueId::BINANCE: return "binance";
            case VenueId::OKX: return "okx";
            case VenueId::BYBIT: return "bybit";
            case VenueId::BITGET: return "bitget";
            case VenueId::KUCOIN: return "kucoin";
            default: return "UNKNOWN";
        }
    }

    /**
     * @brief Minimal configuration for a venue feed.
     *
     * Extend options:
     *  - host, port, ws_target (e.g., "/ws/btcusdt@aggTrade")
     *  - multiple symbols/channels (orderbook, trades, tickers)
     *  - timeouts, heartbeat, backoff policy
     */
    struct FeedHandlerConfig {
        VenueId venue_name; ///< e.g. VenueId::BINANCE

        std::string symbol; ///< Symbol normalized to venue requirements
        std::string base_ccy; ///< e.g. "BTC"
        std::string quote_ccy; ///< e.g. "USDT"

        std::string ws_host; ///< optional override, "" = default
        std::string ws_port; ///< optional override, "" = default
        std::string ws_path; ///< optional override, "" = default

        std::string rest_host; ///< optional override, "" = default
        std::string rest_port; ///< optional override, "" = default
        std::string rest_path; ///< optional override, "" = default

        // Optional: PoP -> brain outbound websocket (central aggregation).
        // Disabled when brain_ws_host is empty.
        std::string brain_ws_host;
        std::string brain_ws_port;
        std::string brain_ws_path;
        bool brain_ws_insecure{false}; // testing only: disable TLS cert/host verification

        std::string persist_path; ///< optional event persistence file path, "" = disabled
        std::size_t persist_book_every_updates{0}; ///< 0 = disabled
        std::size_t persist_book_top{0}; ///< top N levels per side for book_state

        int rest_timeout_ms{8000}; ///< REST snapshot request timeout in milliseconds

        size_t depthLevel{0};
    };

    /**
     * @brief Abstract interface for a venue-specific feed handler.
     *
     * Lifecycle (single-threaded strand suggested):
     *   1) init(cfg)   : validate config, capture references (ioc, clients), prep subscriptions. Non-blocking preferred.
     *   2) start()     : enqueue async ops on the provided io_context (e.g., resolve→connect→handshake→subscribe).
     *   3) stop()      : close sockets / cancel timers. Idempotent and safe at any time.
     *
     * Contract:
     *   - init(...) must be called exactly once before start().
     *   - start() may be called once; repeated calls should return ERROR or no-op safely.
     *   - stop() is idempotent; it must not throw; it should cause on-close/health updates as appropriate.
     */
    struct IVenueFeedHandler {
        virtual ~IVenueFeedHandler() = default;

        /// Prepare resources and validate configuration. Should NOT block on network.
        virtual FeedOpResult init(const FeedHandlerConfig &) = 0;

        /// Enqueue the async network chain on the (externally-driven) io_context.
        virtual FeedOpResult start() = 0;

        /// Gracefully stop: close sockets and cancel timers. Safe to call multiple times.
        virtual FeedOpResult stop() = 0;
    };
} // namespace md
