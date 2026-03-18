#pragma once

#include <boost/asio/io_context.hpp>
#include <deque>
#include <memory>
#include <variant>
#include <atomic>
#include <string_view>
#include <cstdint>

#include "abstract/FeedHandler.hpp"
#include "connection_handler/WsClient.hpp"
#include "connection_handler/RestClient.hpp"
#include "orderbook/OrderBookController.hpp"
#include "md/VenueAdapter.hpp"
#include "postprocess/FilePersistSink.hpp"
#include "postprocess/WsPublishSink.hpp"

namespace md {
    /**
     * @brief Venue-agnostic feed orchestrator built on top of the adapter layer.
     *
     * Responsibilities:
     *  - resolve venue-specific endpoints and subscription frames from the selected adapter
     *  - establish and maintain REST/WS connectivity
     *  - drive snapshot + incremental synchronization into `OrderBookController`
     *  - persist normalized events/checkpoints when configured
     *  - recover from disconnects, parse/apply failures, and stale silent sockets
     *
     * Design notes:
     *  - hot-path message parsing stays adapter-specific
     *  - sync/reconnect/watchdog policy stays centralized here
     *  - one instance manages exactly one `(venue, symbol)` feed
     *
     * High-level flow:
     *  1. `init(cfg)` selects the adapter and resolves cold-path runtime data.
     *  2. `start()` connects WS directly or performs venue bootstrap first.
     *  3. snapshot/incremental events are normalized and applied to the controller.
     *  4. if continuity/checksum/watchdog fails, `restartSync()` resets local state and reconnects.
     */
    class GenericFeedHandler final : public IVenueFeedHandler {
    public:
        /// Construct the handler on an externally owned `io_context`.
        explicit GenericFeedHandler(boost::asio::io_context &ioc);

        /**
         * @brief Prepare adapter/runtime state for one venue feed.
         *
         * This is a cold-path setup step. It does not perform network I/O.
         */
        FeedOpResult init(const FeedHandlerConfig &cfg) override;

        /**
         * @brief Start the async feed lifecycle.
         *
         * Depending on venue capabilities this either:
         *  - connects directly to the websocket, or
         *  - performs an HTTP bootstrap first (e.g. KuCoin).
         */
        FeedOpResult start() override;

        /// Stop sockets/timers and reset runtime state.
        FeedOpResult stop() override;

    private:
        /// Concrete adapter selected from `FeedHandlerConfig::venue_name`.
        using AnyAdapter = std::variant<BinanceAdapter, OKXAdapter, BitgetAdapter, BybitAdapter, KucoinAdapter>;

        static AnyAdapter makeAdapter(VenueId v);

        /**
         * Runtime values resolved once during `init()`.
         *
         * This avoids re-reading config or recomputing venue-specific details in
         * the message hot path.
         */
        struct RuntimeResolved {
            VenueId venue{};
            std::size_t depth{0};

            EndPoint ws;
            EndPoint rest;

            std::string wsSubscribeFrame;
            std::string restSnapshotTarget;

            VenueCaps caps;

            int ws_ping_interval_ms{0}; ///< app-level websocket ping cadence
            int ws_ping_timeout_ms{0}; ///< venue-provided ping timeout hint if available
            int ws_stale_after_ms{0}; ///< no-message threshold before forced resync
        };

        /**
         * Synchronization state machine for one venue stream.
         *
         * Rest-anchored venues:
         *  CONNECTING -> WAIT_REST_SNAPSHOT -> WAIT_BRIDGE -> SYNCED
         *
         * WS-authoritative venues:
         *  CONNECTING -> WAIT_WS_SNAPSHOT -> WAIT_BRIDGE/SYNCED
         *
         * `WAIT_BRIDGE` means a baseline exists but the controller is not yet
         * ready to accept the stream as synchronized.
         */
        enum class FeedSyncState : std::uint8_t {
            DISCONNECTED,
            CONNECTING,
            BOOTSTRAPPING,

            WAIT_REST_SNAPSHOT, // RestAnchored: WS open, buffering incrementals, REST snapshot in-flight
            WAIT_WS_SNAPSHOT, // WsAuthoritative: WS open, waiting for WS snapshot
            WAIT_BRIDGE, // RestAnchored: REST snapshot loaded, draining buffer until first “bridging” update applied

            SYNCED
        };

        /// Open the websocket using the already resolved runtime endpoint.
        void connectWS();

        /// Send the subscribe frame and transition into the venue's sync mode.
        void onWSOpen();

        /// Main raw websocket message entry point.
        void onWSMessage(const char *data, std::size_t len);

        /// Request REST snapshot for rest-anchored venues.
        void requestSnapshot();

        /// Parse/apply REST snapshot and attempt buffered bridge.
        void onSnapshotResponse(std::string_view body);

        /// Reset local sync state and schedule reconnect/rebootstrap.
        void restartSync(std::string_view reason);

        /// Drain buffered incrementals through the normal apply path.
        void drainBufferedIncrementals();

        /// Perform venue-specific bootstrap before websocket connect if required.
        void bootstrapWS();

        std::string makeConnectId() const;
        static std::int64_t now_ns_() noexcept;
        static const char *sync_state_to_string_(FeedSyncState state) noexcept;
        void set_state_(FeedSyncState next, std::string_view reason);

        /// Handle websocket close callback and decide whether to resync.
        void onWSClose_();

        /// Schedule the next connect attempt after a controlled delay.
        void schedule_ws_reconnect_(std::chrono::milliseconds delay);

        /**
         * Refresh the silent-socket watchdog.
         *
         * The watchdog detects the "process still alive but feed stopped moving"
         * failure mode where no close/error event is delivered by the socket.
         */
        void arm_ws_watchdog_();

        /// Cancel any outstanding stale-feed watchdog callback.
        void disarm_ws_watchdog_();

        void persist_snapshot_(const GenericSnapshotFormat &snap, std::string_view source);
        void persist_incremental_(const GenericIncrementalFormat &inc, std::string_view source);
        void maybe_persist_book_(std::string_view source);

    private:
        boost::asio::io_context &ioc_;
        std::shared_ptr<WsClient> ws_;
        std::shared_ptr<RestClient> rest_;

        std::string connect_id_;

        std::unique_ptr<OrderBookController> controller_;
        std::unique_ptr<FilePersistSink> persist_;
        std::unique_ptr<WsPublishSink> brain_publish_;

        FeedHandlerConfig cfg_; /// DO NOT READ IN HOT PATH
        RuntimeResolved rt_;
        AnyAdapter adapter_;

        std::atomic<bool> running_{false};
        FeedSyncState state_{FeedSyncState::DISCONNECTED};

        /// Buffered incrementals captured before a valid baseline is ready.
        struct BufferedMsg {
            std::string payload;
            std::int64_t recv_ts_ns{0};
        };
        std::deque<BufferedMsg> buffer_; /// later optimize to ring buffer / pooled storage
        std::size_t max_buffer_{10'000};

        boost::asio::steady_timer reconnect_timer_; ///< delayed reconnect/backoff timer
        boost::asio::steady_timer ws_watchdog_timer_; ///< detects silent stale sockets
        boost::asio::steady_timer heartbeat_timer_; ///< periodic stats heartbeat
        std::uint64_t reconnect_gen_{0}; ///< invalidates old reconnect callbacks
        std::uint64_t ws_watchdog_gen_{0}; ///< invalidates old watchdog callbacks
        bool reconnect_scheduled_{false};

        // B6: exponential reconnect backoff state
        int reconnect_delay_ms_{1000}; ///< current backoff delay; doubles on each failure, capped at 60 s
        static constexpr int kReconnectInitMs = 1'000;
        static constexpr int kReconnectMaxMs  = 60'000;
        /// Returns current delay with ±25 % jitter then doubles for next call.
        std::chrono::milliseconds next_reconnect_delay_() noexcept;
        /// Resets backoff to initial value (called on successful SYNCED transition).
        void reset_reconnect_delay_() noexcept;
        bool closing_for_restart_{false}; ///< suppresses duplicate restart on self-initiated close
        bool ws_watchdog_announced_{false};
        std::int64_t last_ws_message_ns_{0}; ///< last raw websocket frame receive time
        std::size_t persist_book_every_updates_{0};
        std::size_t persist_book_top_{0};
        std::size_t updates_since_book_persist_{0};

        // B6 / C4: REST request timing
        std::int64_t rest_request_start_ns_{0}; ///< when requestSnapshot() was last called

        // D2: per-feed counters
        std::uint64_t ctr_msgs_received_{0};   ///< raw WS frames received since last heartbeat
        std::uint64_t ctr_resyncs_{0};         ///< total restartSync() calls
        std::uint64_t ctr_book_updates_{0};    ///< total applied incremental updates
        std::uint64_t ctr_outbox_drops_{0};    ///< WsPublishSink outbox overflow drops (approximation)

        void arm_heartbeat_();
        void emit_heartbeat_();
    };
}
