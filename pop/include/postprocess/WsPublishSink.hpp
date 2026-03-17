#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "connection_handler/WsClient.hpp"
#include "orderbook/OrderBookController.hpp"

namespace md {
    /**
     * Publishes normalized feed events to a central "brain" over an outbound WebSocket
     * (TLS by default).  Payloads are identical to the JSONL records written by
     * FilePersistSink but are sent as individual WebSocket text frames (not newline-
     * delimited).
     *
     * Lifecycle:
     *   - Disabled (no-op) until start() is called.
     *   - Automatically reconnects with exponential backoff (1 s → 30 s) on drop.
     *   - stop() cancels any pending reconnect and closes the socket.
     *
     * Backpressure:
     *   - Outgoing messages are queued in WsClient's outbox (capped at kDefaultMaxOutbox).
     *   - When the cap is exceeded, the *oldest* messages are dropped (prefer freshness).
     *
     * Sequence numbering:
     *   - persist_seq is an independent counter; it is not shared with FilePersistSink.
     *     If both sinks are active their persist_seq spaces are disjoint.
     *
     * TLS:
     *   - Peer verification is enabled by default.
     *   - Pass insecure_tls=true to disable cert/host checking (local testing only).
     *     A warning is emitted to stderr on every connection when this flag is set.
     */
    class WsPublishSink {
    public:
        WsPublishSink(boost::asio::io_context &ioc,
                      std::string host,
                      std::string port,
                      std::string target,
                      bool insecure_tls,
                      std::string venue,
                      std::string symbol);

        void start();
        void stop();

        /// Publish a feed health status event to brain.
        /// feed_state: "disconnected" | "resyncing" | "synced"
        /// Called automatically by GenericFeedHandler on every sync state transition.
        void publish_status(std::string_view feed_state, std::string_view reason) noexcept;

        void publish_snapshot(const GenericSnapshotFormat &snap, std::string_view source) noexcept;
        void publish_incremental(const GenericIncrementalFormat &inc, std::string_view source) noexcept;
        void publish_book_state(const OrderBook &book,
                                std::uint64_t applied_seq,
                                std::size_t top_n,
                                std::string_view source,
                                std::int64_t ts_book_ns) noexcept;

    private:
        static std::int64_t now_ns_() noexcept;
        static nlohmann::json levels_to_json_(const std::vector<Level> &levels);
        static nlohmann::json levels_from_book_(const OrderBook &book, std::size_t top_n, Side side);

        void connect_();
        void schedule_reconnect_();

        void send_json_(const nlohmann::json &j) noexcept;

    private:
        boost::asio::io_context &ioc_;
        std::shared_ptr<WsClient> ws_;
        boost::asio::steady_timer reconnect_timer_;

        std::string host_;
        std::string port_;
        std::string target_;
        std::string venue_;
        std::string symbol_;

        bool insecure_tls_{false};
        bool running_{false};
        bool reconnect_scheduled_{false};
        int reconnect_delay_ms_{1000};
        std::uint64_t reconnect_gen_{0};

        // Independent counter; not shared with FilePersistSink.
        // If both sinks are active, their persist_seq spaces are disjoint.
        std::uint64_t persist_seq_{0};
    };
} // namespace md
