#include "md/GenericFeedHandler.hpp"
#include <chrono>
#include <iostream>

namespace md
{
    namespace
    {
        constexpr int kDefaultWsPingIntervalMs = 15'000;
        constexpr int kMinWsStaleAfterMs = 30'000;
    }

    GenericFeedHandler::GenericFeedHandler(boost::asio::io_context &ioc) : ioc_(ioc),
                                                                           ws_(WsClient::create(ioc)),
                                                                           rest_(RestClient::create(ioc)),
                                                                           reconnect_timer_(ioc),
                                                                           ws_watchdog_timer_(ioc)
    {
        rest_->set_keep_alive(true); // strongly recommended for snapshots
        rest_->set_logger([](std::string_view s)
                          {
            std::cerr << s << "\n"; });
    }

    std::string GenericFeedHandler::makeConnectId() const
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        return std::to_string(ms);
    }

    std::int64_t GenericFeedHandler::now_ns_() noexcept
    {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    }

    const char *GenericFeedHandler::sync_state_to_string_(FeedSyncState state) noexcept
    {
        switch (state)
        {
        case FeedSyncState::DISCONNECTED:
            return "DISCONNECTED";
        case FeedSyncState::CONNECTING:
            return "CONNECTING";
        case FeedSyncState::BOOTSTRAPPING:
            return "BOOTSTRAPPING";
        case FeedSyncState::WAIT_REST_SNAPSHOT:
            return "WAIT_REST_SNAPSHOT";
        case FeedSyncState::WAIT_WS_SNAPSHOT:
            return "WAIT_WS_SNAPSHOT";
        case FeedSyncState::WAIT_BRIDGE:
            return "WAIT_BRIDGE";
        case FeedSyncState::SYNCED:
            return "SYNCED";
        default:
            return "UNKNOWN";
        }
    }

    void GenericFeedHandler::set_state_(FeedSyncState next, std::string_view reason)
    {
        if (state_ == next)
            return;
        std::cerr << "[GenericFeedHandler] state "
                  << sync_state_to_string_(state_)
                  << " -> " << sync_state_to_string_(next)
                  << " venue=" << to_string(rt_.venue);
        if (!reason.empty())
        {
            std::cerr << " reason=" << reason;
        }
        std::cerr << "\n";
        state_ = next;

        if (brain_publish_) {
            std::string_view feed_state;
            switch (next) {
                case FeedSyncState::DISCONNECTED: feed_state = "disconnected"; break;
                case FeedSyncState::SYNCED:       feed_state = "synced";       break;
                default:                          feed_state = "resyncing";    break;
            }
            brain_publish_->publish_status(feed_state, reason);
        }
    }

    GenericFeedHandler::AnyAdapter GenericFeedHandler::makeAdapter(VenueId v)
    {
        switch (v)
        {
        case VenueId::BINANCE:
            return BinanceAdapter{};
        case VenueId::OKX:
            return OKXAdapter{};
        case VenueId::BITGET:
            return BitgetAdapter{};
        case VenueId::BYBIT:
            return BybitAdapter{};
        case VenueId::KUCOIN:
            return KucoinAdapter{};
        default:
            return BinanceAdapter{};
        }
    }

    FeedOpResult GenericFeedHandler::init(const FeedHandlerConfig &cfg)
    {
        if (running_.load())
            return FeedOpResult::ERROR;
        if (cfg.depthLevel == 0)
            return FeedOpResult::ERROR;

        cfg_ = cfg;

        rest_->set_timeout(std::chrono::milliseconds(cfg_.rest_timeout_ms));
        rest_->set_shutdown_timeout(std::chrono::milliseconds(2000));

        adapter_ = makeAdapter(cfg.venue_name);
        rt_.caps = std::visit([&](auto const &a)
                              { return a.caps(); }, adapter_);

        rt_.venue = cfg_.venue_name;
        rt_.depth = cfg_.depthLevel;

        /// Resolve endpoints + prebuild frames/targets once (Cold Path)
        rt_.ws = std::visit([&](auto const &a)
                            { return a.wsEndpoint(cfg_); }, adapter_);
        rt_.rest = std::visit([&](auto const &a)
                              { return a.restEndpoint(cfg_); }, adapter_);

        rt_.wsSubscribeFrame = std::visit([&](auto const &a)
                                          { return a.wsSubscribeFrame(cfg_); }, adapter_);
        rt_.restSnapshotTarget = std::visit([&](auto const &a)
                                            { return a.restSnapshotTarget(cfg_); }, adapter_);
        rt_.ws_ping_interval_ms = kDefaultWsPingIntervalMs;
        rt_.ws_stale_after_ms = kMinWsStaleAfterMs;

        if (!cfg_.rest_host.empty())
            rt_.rest.host = cfg_.rest_host;
        if (!cfg_.rest_port.empty())
            rt_.rest.port = cfg_.rest_port;
        if (!cfg_.rest_path.empty())
            rt_.restSnapshotTarget = cfg_.rest_path;

        controller_ = std::make_unique<OrderBookController>(rt_.depth);
        controller_->configureChecksum(rt_.caps.checksum_fn, rt_.caps.checksum_top_n);
        // KuCoin and a few other venues may emit non-contiguous sequence numbers
        // especially when using partial REST snapshots.  Allow controller to
        // tolerate gaps if requested by the adapter via VenueCaps.
        controller_->setAllowSequenceGap(rt_.caps.allow_seq_gap);
        if (rt_.caps.allow_seq_gap)
        {
            std::cerr << "[GenericFeedHandler] ALLOW_SEQ_GAP enabled for venue\n";
        }

        persist_.reset();
        // Book checkpoint cadence is shared by file persistence and brain publishing.
        persist_book_every_updates_ = cfg_.persist_book_every_updates;
        persist_book_top_ = cfg_.persist_book_top > 0 ? cfg_.persist_book_top : rt_.depth;
        updates_since_book_persist_ = 0;
        if (!cfg_.persist_path.empty())
        {
            persist_ = std::make_unique<FilePersistSink>(cfg_.persist_path, to_string(rt_.venue), cfg_.symbol);
            if (!persist_->is_open())
            {
                std::cerr << "[GenericFeedHandler] WARN: failed to open persistence sink at " << cfg_.persist_path << "\n";
                persist_.reset();
            }
            else
            {
                std::cerr << "[GenericFeedHandler] persistence enabled: " << cfg_.persist_path << "\n";
                // cadence already populated above
            }
        }

        brain_publish_.reset();
        if (!cfg_.brain_ws_host.empty())
        {
            const std::string host = cfg_.brain_ws_host;
            const std::string port = cfg_.brain_ws_port;  // defaults applied in main.cpp
            const std::string path = cfg_.brain_ws_path;  // defaults applied in main.cpp
            brain_publish_ = std::make_unique<WsPublishSink>(ioc_,
                                                            host,
                                                            port,
                                                            path,
                                                            cfg_.brain_ws_insecure,
                                                            to_string(rt_.venue),
                                                            cfg_.symbol);
            std::cerr << "[GenericFeedHandler] brain publish enabled: " << host << ":" << port << path << "\n";
        }

        buffer_.clear();
        set_state_(FeedSyncState::DISCONNECTED, "init");
        last_ws_message_ns_ = 0;
        ws_watchdog_gen_ = 0;
        ws_watchdog_announced_ = false;

        if (rt_.ws_ping_timeout_ms > 0)
        {
            rt_.ws_stale_after_ms = std::max(rt_.ws_ping_timeout_ms, kMinWsStaleAfterMs);
        }
        else if (rt_.ws_ping_interval_ms > 0)
        {
            rt_.ws_stale_after_ms = std::max(rt_.ws_ping_interval_ms * 3, kMinWsStaleAfterMs);
        }

        return FeedOpResult::OK;
    }

    FeedOpResult GenericFeedHandler::start()
    {
        if (running_.exchange(true))
        {
            return FeedOpResult::ERROR;
        }

        set_state_(FeedSyncState::CONNECTING, "start");

        /// Wire WS callback once
        ws_->set_on_open([this]
                         { onWSOpen(); });
        ws_->set_on_raw_message([this](const char *data, std::size_t len)
                                { onWSMessage(data, len); });
        ws_->set_on_close([this]
                          {
            if (!running_.load(std::memory_order_acquire)) return;
            onWSClose_(); });

        connect_id_ = makeConnectId();

        if (brain_publish_) brain_publish_->start();

        if (rt_.caps.requires_ws_bootstrap)
        {
            set_state_(FeedSyncState::BOOTSTRAPPING, "venue_bootstrap_required");
            bootstrapWS();
            return FeedOpResult::OK;
        }

        connectWS();
        return FeedOpResult::OK;
    }

    FeedOpResult GenericFeedHandler::stop()
    {
        running_.store(false, std::memory_order_release);

        if (rest_)
            rest_->cancel();
        if (ws_)
            ws_->close();
        if (brain_publish_)
            brain_publish_->stop();
        disarm_ws_watchdog_();

        set_state_(FeedSyncState::DISCONNECTED, "stop");
        buffer_.clear();

        if (controller_)
            controller_->resetBook();
        persist_.reset();
        brain_publish_.reset();
        persist_book_every_updates_ = 0;
        persist_book_top_ = 0;
        updates_since_book_persist_ = 0;
        last_ws_message_ns_ = 0;
        ws_watchdog_announced_ = false;
        return FeedOpResult::OK;
    }

    /**
     * - Generic WS connect uses resolved endpoint
     */
    void GenericFeedHandler::connectWS()
    {
        if (rt_.ws_ping_interval_ms > 0)
        {
            ws_->set_idle_ping(std::chrono::milliseconds(rt_.ws_ping_interval_ms));
        }
        else
        {
            ws_->set_idle_ping(std::chrono::milliseconds(0));
        }
        std::cerr << "[GenericFeedHandler] connecting websocket"
                  << " venue=" << to_string(rt_.venue)
                  << " host=" << rt_.ws.host
                  << " port=" << rt_.ws.port
                  << " target=" << rt_.ws.target
                  << " ping_interval_ms=" << rt_.ws_ping_interval_ms
                  << " stale_after_ms=" << rt_.ws_stale_after_ms
                  << "\n";
        ws_->connect(rt_.ws.host, rt_.ws.port, rt_.ws.target);
    }

    /**
     * Subscribe to stream
     */
    void GenericFeedHandler::onWSOpen()
    {
        if (!running_.load())
            return;

        last_ws_message_ns_ = now_ns_();
        arm_ws_watchdog_();

        std::cerr << "[GenericFeedHandler] websocket connected"
                  << " venue=" << to_string(rt_.venue)
                  << " host=" << rt_.ws.host
                  << " port=" << rt_.ws.port
                  << " target=" << rt_.ws.target
                  << "\n";

        if (!rt_.wsSubscribeFrame.empty())
        {
            std::cerr << "[GenericFeedHandler] sending ws subscribe"
                      << " venue=" << to_string(rt_.venue)
                      << " payload_bytes=" << rt_.wsSubscribeFrame.size()
                      << "\n";
            ws_->send_text(rt_.wsSubscribeFrame);
        }

        if (rt_.caps.sync_mode == SyncMode::RestAnchored)
        {
            set_state_(FeedSyncState::WAIT_REST_SNAPSHOT, "ws_open_rest_anchored");
            std::cerr << "[GenericFeedHandler] requesting REST snapshot"
                      << " venue=" << to_string(rt_.venue)
                      << "\n";
            requestSnapshot();
        }
        else
        {
            set_state_(FeedSyncState::WAIT_WS_SNAPSHOT, "ws_open_ws_authoritative");
            std::cerr << "[GenericFeedHandler] waiting for WS snapshot"
                      << " venue=" << to_string(rt_.venue)
                      << "\n";
        }
    }

    /**
     * - Async GET snapshot
     */
    void GenericFeedHandler::requestSnapshot()
    {
        set_state_(FeedSyncState::WAIT_REST_SNAPSHOT, "request_snapshot");
        std::cerr << "[GenericFeedHandler] requesting REST snapshot"
                  << " venue=" << to_string(rt_.venue)
                  << " host=" << rt_.rest.host
                  << " port=" << rt_.rest.port
                  << " target=" << rt_.restSnapshotTarget
                  << "\n";

        rest_->async_get(rt_.rest.host, rt_.rest.port, rt_.restSnapshotTarget,
                         [this](boost::system::error_code ec, std::string body)
                         {
                             if (!running_.load(std::memory_order_acquire))
                                 return;

                             if (ec)
                             {
                                 // network / TLS / timeout errors
                                 restartSync("rest_snapshot_transport_error");
                                 return;
                             }

                             const int status = rest_->last_http_status();

                             if (status == 429 || status == 418)
                             {
                                 std::cerr << "[GenericFeedHandler] REST snapshot rate limited"
                                           << " venue=" << to_string(rt_.venue)
                                           << " status=" << status
                                           << " retry_ms=750"
                                           << "\n";
                                 /// Rate-limited / temporary ban -> do NOT hammer.
                                 /// Simple fixed delay; replace with exponential backoff later.
                                 reconnect_timer_.expires_after(std::chrono::milliseconds(750));
                                 reconnect_timer_.async_wait([this](const boost::system::error_code &ec)
                                                             {
                                     if (ec) return;
                                     if (!running_.load()) return;
                                     requestSnapshot(); });
                                 return;
                             }

                             if (status < 200 || status >= 300)
                             {
                                 // 4xx/5xx -> handle separately if you want
                                 restartSync("rest_snapshot_http_error");
                                 return;
                             }

                             onSnapshotResponse(body);
                         });
    }

    void GenericFeedHandler::onSnapshotResponse(std::string_view body)
    {
        if (!running_.load())
            return;
        const std::int64_t recv_ts_ns = now_ns_();
        std::cerr << "[GenericFeedHandler] received REST snapshot response"
                  << " venue=" << to_string(rt_.venue)
                  << " body_bytes=" << body.size()
                  << "\n";

        GenericSnapshotFormat snap;
        const bool ok = std::visit([&](auto const &a)
                                   { return a.parseSnapshot(body, snap); }, adapter_);

        if (!ok)
        {
            restartSync("rest_snapshot_parse_failed");
            return;
        }
        snap.ts_recv_ns = recv_ts_ns;

        persist_snapshot_(snap, "rest_snapshot");

        const auto kind =
            (rt_.caps.sync_mode == SyncMode::RestAnchored)
                ? OrderBookController::BaselineKind::RestAnchored
                : OrderBookController::BaselineKind::WsAuthoritative;

        controller_->onSnapshot(snap, kind);
        maybe_persist_book_("snapshot_applied");

        /// Baseline loaded. We are not necessarily synced yet (RestAnchored must bridge).
        set_state_(FeedSyncState::WAIT_BRIDGE, "snapshot_applied");

        /// Apply buffered incrementals after snapshot
        drainBufferedIncrementals();

        if (controller_->isSynced())
        {
            std::cerr << "[GenericFeedHandler] bridged (post-snapshot drain) -> SYNCED\n";
            set_state_(FeedSyncState::SYNCED, "post_snapshot_bridge_complete");
        }
        else
        {
            std::cerr << "[GenericFeedHandler] still WAIT_BRIDGE after drain\n";
        }
    }

    void GenericFeedHandler::drainBufferedIncrementals()
    {
        while (!buffer_.empty())
        {
            GenericIncrementalFormat inc;

            const BufferedMsg buffered = std::move(buffer_.front());

            const bool ok = std::visit([&](auto const &a)
                                       {
                if (!a.isIncremental(buffered.payload)) return false;
                return a.parseIncremental(buffered.payload, inc); }, adapter_);

            buffer_.pop_front();

            if (!ok)
                continue;
            inc.ts_recv_ns = buffered.recv_ts_ns;

            const auto action = controller_->onIncrement(inc);
            persist_incremental_(inc, "ws_incremental");
            if (action == OrderBookController::Action::NeedResync)
            {
                restartSync("buffered_incremental_need_resync");
                return;
            }
            maybe_persist_book_("incremental_applied");
        }
    }

    void GenericFeedHandler::onWSMessage(const char *data, std::size_t len)
    {
        if (!running_.load() || len == 0)
            return;
        std::string_view msg{data, len};
        const std::int64_t recv_ts_ns = now_ns_();
        last_ws_message_ns_ = recv_ts_ns;
        arm_ws_watchdog_();

        if (state_ == FeedSyncState::WAIT_REST_SNAPSHOT)
        {
            // buffer incrementals
            const bool isInc = std::visit([&](auto const &a)
                                          { return a.isIncremental(msg); }, adapter_);
            if (isInc)
            {
                if (buffer_.size() < max_buffer_)
                    buffer_.push_back(BufferedMsg{std::string(msg), recv_ts_ns});
                else
                    restartSync("buffer_overflow_wait_rest_snapshot");
            }
            return;
        }

        if (state_ == FeedSyncState::WAIT_WS_SNAPSHOT)
        {
            // first, try snapshot
            GenericSnapshotFormat snap;
            const bool isSnap = std::visit([&](auto const &a)
                                           { return a.isSnapshot(msg) && a.parseWsSnapshot(msg, snap); }, adapter_);

            if (isSnap)
            {
                snap.ts_recv_ns = recv_ts_ns;
                persist_snapshot_(snap, "ws_snapshot");
                controller_->onSnapshot(snap, OrderBookController::BaselineKind::WsAuthoritative);
                maybe_persist_book_("snapshot_applied");

                // baseline is WS snapshot; any buffered msgs were pre-baseline, drain them now
                state_ = FeedSyncState::WAIT_BRIDGE;
                drainBufferedIncrementals();
                if (controller_->isSynced())
                    set_state_(FeedSyncState::SYNCED, "ws_snapshot_baseline_ready");
                return;
            }

            // otherwise buffer incrementals
            const bool isInc = std::visit([&](auto const &a)
                                          { return a.isIncremental(msg); }, adapter_);
            if (isInc)
            {
                if (buffer_.size() < max_buffer_)
                    buffer_.push_back(BufferedMsg{std::string(msg), recv_ts_ns});
                else
                    restartSync("buffer_overflow_wait_ws_snapshot");
            }
            return;
        }

        // WAIT_BRIDGE and SYNCED:
        // 1) For WS-authoritative venues, allow an "interrupting" WS snapshot at ANY time and re-baseline.
        if ((state_ == FeedSyncState::WAIT_BRIDGE || state_ == FeedSyncState::SYNCED) && rt_.caps.ws_sends_snapshot)
        {
            GenericSnapshotFormat snap;
            const bool isSnap = std::visit([&](auto const &a)
                                           { return a.isSnapshot(msg) && a.parseWsSnapshot(msg, snap); }, adapter_);

            if (isSnap)
            {
                // Hard re-baseline (venue may resend snapshot on internal resync)
                snap.ts_recv_ns = recv_ts_ns;
                persist_snapshot_(snap, "ws_snapshot");
                controller_->onSnapshot(snap, OrderBookController::BaselineKind::WsAuthoritative);
                maybe_persist_book_("snapshot_applied");

                // Any buffered incrementals are stale relative to this new baseline.
                buffer_.clear();

                // WS-authoritative snapshot implies we can treat it as baseline-loaded immediately.
                // Controller may set Synced directly; keep handler consistent.
                set_state_(controller_->isSynced() ? FeedSyncState::SYNCED : FeedSyncState::WAIT_BRIDGE,
                           "interrupting_ws_snapshot");
                return;
            }
        }

        // 2) Otherwise: parse + apply incrementals
        if (state_ == FeedSyncState::WAIT_BRIDGE || state_ == FeedSyncState::SYNCED)
        {
            // --- RestAnchored: during WAIT_BRIDGE we ONLY buffer+drain ---
            if (rt_.caps.sync_mode == SyncMode::RestAnchored && state_ == FeedSyncState::WAIT_BRIDGE)
            {
                const bool isInc = std::visit([&](auto const &a)
                                              { return a.isIncremental(msg); }, adapter_);
                if (!isInc)
                    return;

                if (buffer_.size() < max_buffer_)
                    buffer_.push_back(BufferedMsg{std::string(msg), recv_ts_ns});
                else
                {
                    restartSync("buffer_overflow_wait_bridge");
                    return;
                }

                // Try to bridge using the same pipeline as post-snapshot drain
                drainBufferedIncrementals();
                if (controller_->isSynced())
                {
                    std::cerr << "[GenericFeedHandler] bridged (ws buffered path) -> SYNCED\n";
                    set_state_(FeedSyncState::SYNCED, "ws_buffered_bridge_complete");
                }
                return;
            }

            // --- Otherwise: steady-state apply (SYNCED, or WS-authoritative venues) ---
            GenericIncrementalFormat inc;
            const bool ok = std::visit([&](auto const &a)
                                       {
                if (!a.isIncremental(msg)) return false;
                return a.parseIncremental(msg, inc); }, adapter_);

            if (!ok)
                return;
            inc.ts_recv_ns = recv_ts_ns;

            const auto action = controller_->onIncrement(inc);
            persist_incremental_(inc, "ws_incremental");
            if (action == OrderBookController::Action::NeedResync)
            {
                restartSync("steady_state_incremental_need_resync");
                return;
            }
            maybe_persist_book_("incremental_applied");

            if (state_ == FeedSyncState::WAIT_BRIDGE && controller_->isSynced())
            {
                std::cerr << "[GenericFeedHandler] bridged (ws path) -> SYNCED\n";
                set_state_(FeedSyncState::SYNCED, "steady_state_bridge_complete");
            }
            return;
        }

        // Any other state: ignore
        return;
    }

    void GenericFeedHandler::restartSync(std::string_view reason)
    {
        if (!running_.load())
            return;

        std::cerr << "[GenericFeedHandler] restart sync"
                  << " venue=" << to_string(rt_.venue)
                  << " state=" << sync_state_to_string_(state_);
        if (!reason.empty())
        {
            std::cerr << " reason=" << reason;
        }
        std::cerr << "\n";

        disarm_ws_watchdog_();
        buffer_.clear();
        controller_->resetBook();

        // Reset state before reconnect
        set_state_(FeedSyncState::CONNECTING, "restart_sync");

        // Correlate bootstrap if needed
        connect_id_ = makeConnectId();

        // Force-close current WS (immediate) and reconnect after a short backoff.
        closing_for_restart_ = true;
        ws_->cancel();

        schedule_ws_reconnect_(std::chrono::milliseconds(200));
    }

    void GenericFeedHandler::bootstrapWS()
    {
        if (!running_.load())
            return;

        const std::string target = std::visit([&](auto const &a)
                                              { return a.wsBootstrapTarget(cfg_); }, adapter_);

        if (target.empty())
        {
            // caps say we require bootstrap but adapter can't provide it -> hard fail
            std::cerr << "[GenericFeedHandler] ERROR: venue requires WS bootstrap but adapter did not provide target\n";
            restartSync("bootstrap_missing_target");
            return;
        }

        const std::string body = std::visit([&](auto const &a)
                                            { return a.wsBootstrapBody(cfg_); }, adapter_);

        std::cerr << "[GenericFeedHandler] requesting ws bootstrap"
                  << " venue=" << to_string(rt_.venue)
                  << " host=" << rt_.rest.host
                  << " port=" << rt_.rest.port
                  << " target=" << target
                  << " body_bytes=" << body.size()
                  << "\n";

        // POST bullet-public
        rest_->async_post(rt_.rest.host, rt_.rest.port, target, body,
                          [this](boost::system::error_code ec, const std::string &resp_body)
                          {
                              if (ec)
                              {
                                  restartSync("ws_bootstrap_transport_error");
                                  return;
                              }

                              WsBootstrapInfo info;
                              const bool ok = std::visit([&](auto const &a)
                                                         { return a.parseWsBootstrap(resp_body, connect_id_, info); }, adapter_);

                              if (!ok)
                              {
                                  restartSync("ws_bootstrap_parse_failed");
                                  return;
                              }

                              std::cerr << "[GenericFeedHandler] ws bootstrap resolved"
                                        << " venue=" << to_string(rt_.venue)
                                        << " host=" << info.ws.host
                                        << " port=" << info.ws.port
                                        << " target=" << info.ws.target
                                        << " ping_interval_ms=" << info.ping_interval_ms
                                        << " ping_timeout_ms=" << info.ping_timeout_ms
                                        << "\n";

                              // overwrite resolved WS endpoint from bootstrap
                              rt_.ws = info.ws;
                              rt_.ws_ping_interval_ms = info.ping_interval_ms;
                              rt_.ws_ping_timeout_ms = info.ping_timeout_ms;

                              // now connect
                              connectWS();
                          });
    }

    void GenericFeedHandler::onWSClose_()
    {
        disarm_ws_watchdog_();

        // If we initiated the close as part of restartSync(), do NOT re-enter restartSync().
        if (closing_for_restart_)
        {
            closing_for_restart_ = false;
            return;
        }

        // Unexpected close (network flap, remote close, etc.)
        restartSync("unexpected_ws_close");
    }

    void GenericFeedHandler::schedule_ws_reconnect_(std::chrono::milliseconds delay)
    {
        ++reconnect_gen_;
        const auto my_gen = reconnect_gen_;

        std::cerr << "[GenericFeedHandler] scheduling reconnect"
                  << " venue=" << to_string(rt_.venue)
                  << " delay_ms=" << delay.count()
                  << " next_mode=" << (rt_.caps.requires_ws_bootstrap ? "bootstrap" : "connect")
                  << "\n";

        reconnect_scheduled_ = true;
        reconnect_timer_.expires_after(delay);

        reconnect_timer_.async_wait([this, my_gen](const boost::system::error_code &ec)
                                    {
            if (ec) return;
            if (!running_.load()) return;
            if (my_gen != reconnect_gen_) return;

            reconnect_scheduled_ = false;

            if (rt_.caps.requires_ws_bootstrap) {
                set_state_(FeedSyncState::BOOTSTRAPPING, "scheduled_rebootstrap");
                bootstrapWS();
            } else {
                set_state_(FeedSyncState::CONNECTING, "scheduled_reconnect");
                connectWS();
            } });
    }

    void GenericFeedHandler::arm_ws_watchdog_()
    {
        if (rt_.ws_stale_after_ms <= 0)
            return;

        ++ws_watchdog_gen_;
        const auto my_gen = ws_watchdog_gen_;
        if (!ws_watchdog_announced_) {
            std::cerr << "[GenericFeedHandler] feed watchdog active"
                      << " venue=" << to_string(rt_.venue)
                      << " timeout_ms=" << rt_.ws_stale_after_ms
                      << "\n";
            ws_watchdog_announced_ = true;
        }
        ws_watchdog_timer_.expires_after(std::chrono::milliseconds(rt_.ws_stale_after_ms));
        ws_watchdog_timer_.async_wait([this, my_gen](const boost::system::error_code &ec)
                                      {
            if (ec) return;
            if (!running_.load()) return;
            if (my_gen != ws_watchdog_gen_) return;
            if (state_ == FeedSyncState::DISCONNECTED || state_ == FeedSyncState::CONNECTING || state_ == FeedSyncState::BOOTSTRAPPING) {
                return;
            }

            const std::int64_t now = now_ns_();
            const std::int64_t age_ns = now - last_ws_message_ns_;
            if (last_ws_message_ns_ > 0 && age_ns >= static_cast<std::int64_t>(rt_.ws_stale_after_ms) * 1'000'000LL) {
                std::cerr << "[GenericFeedHandler] stale WS feed detected, restarting sync"
                          << " venue=" << to_string(rt_.venue)
                          << " state=" << static_cast<int>(state_)
                          << " silence_ms=" << (age_ns / 1'000'000LL)
                          << "\n";
                restartSync("stale_ws_feed");
            } });
    }

    void GenericFeedHandler::disarm_ws_watchdog_()
    {
        ++ws_watchdog_gen_;
        ws_watchdog_announced_ = false;
        ws_watchdog_timer_.cancel();
    }

    void GenericFeedHandler::persist_snapshot_(const GenericSnapshotFormat &snap, std::string_view source)
    {
        if (persist_) persist_->write_snapshot(snap, source);
        if (brain_publish_) brain_publish_->publish_snapshot(snap, source);
    }

    void GenericFeedHandler::persist_incremental_(const GenericIncrementalFormat &inc, std::string_view source)
    {
        if (persist_) persist_->write_incremental(inc, source);
        if (brain_publish_) brain_publish_->publish_incremental(inc, source);
    }

    void GenericFeedHandler::maybe_persist_book_(std::string_view source)
    {
        if ((!persist_ && !brain_publish_) || !controller_)
            return;
        if (persist_book_every_updates_ == 0 || persist_book_top_ == 0)
            return;
        if (!controller_->isSynced())
            return;

        ++updates_since_book_persist_;
        if (updates_since_book_persist_ < persist_book_every_updates_)
            return;
        updates_since_book_persist_ = 0;

        const auto ts_book_ns = now_ns_();
        if (persist_)
        {
            persist_->write_book_state(controller_->book(),
                                       controller_->getAppliedSeqID(),
                                       persist_book_top_,
                                       source,
                                       ts_book_ns);
        }
        if (brain_publish_)
        {
            brain_publish_->publish_book_state(controller_->book(),
                                               controller_->getAppliedSeqID(),
                                               persist_book_top_,
                                               source,
                                               ts_book_ns);
        }
    }
}
