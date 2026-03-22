#include "md/GenericFeedHandler.hpp"
#include <chrono>
#include <random>
#include <spdlog/spdlog.h>

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
                                                                           ws_watchdog_timer_(ioc),
                                                                           heartbeat_timer_(ioc)
    {
        rest_->set_keep_alive(true); // strongly recommended for snapshots
        rest_->set_logger([](std::string_view s)
                          {
            spdlog::debug("{}", s); });
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
        const FeedSyncState cur = state_.load(std::memory_order_relaxed);
        if (cur == next)
            return;
        if (reason.empty()) {
            spdlog::info("[GFH] state {} -> {} venue={}",
                         sync_state_to_string_(cur), sync_state_to_string_(next), to_string(rt_.venue));
        } else {
            spdlog::info("[GFH] state {} -> {} venue={} reason={}",
                         sync_state_to_string_(cur), sync_state_to_string_(next), to_string(rt_.venue), reason);
        }
        state_.store(next, std::memory_order_relaxed);

        // B6: reset reconnect backoff once we successfully reach SYNCED
        if (next == FeedSyncState::SYNCED) {
            reset_reconnect_delay_();
        }

        if (!brain_sinks_.empty()) {
            std::string_view feed_state;
            switch (next) {
                case FeedSyncState::DISCONNECTED: feed_state = "disconnected"; break;
                case FeedSyncState::SYNCED:       feed_state = "synced";       break;
                default:                          feed_state = "resyncing";    break;
            }
            for (auto &s : brain_sinks_) s->publish_status(feed_state, reason);
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
        controller_->setHasChecksum(rt_.caps.has_checksum);
        if (cfg_.require_checksum) {
            controller_->setRequireChecksum(true);
            spdlog::info("[GFH] require_checksum=true has_checksum={}", rt_.caps.has_checksum);
        }
        // KuCoin and a few other venues may emit non-contiguous sequence numbers
        // especially when using partial REST snapshots.  Allow controller to
        // tolerate gaps if requested by the adapter via VenueCaps.
        controller_->setAllowSequenceGap(rt_.caps.allow_seq_gap);
        if (rt_.caps.allow_seq_gap)
        {
            spdlog::info("[GFH] ALLOW_SEQ_GAP enabled for venue");
        }
        // C3: optional periodic book validation
        if (cfg_.validate_every > 0) {
            controller_->setValidatePeriod(static_cast<std::size_t>(cfg_.validate_every));
            spdlog::info("[GFH] validate_every={}", cfg_.validate_every);
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
                spdlog::warn("[GFH] failed to open persistence sink at {}", cfg_.persist_path);
                persist_.reset();
            }
            else
            {
                spdlog::info("[GFH] persistence enabled: {}", cfg_.persist_path);
                // cadence already populated above
            }
        }

        brain_sinks_.clear();
        auto add_sink_ = [&](const std::string &host, const std::string &port,
                              const std::string &path, bool insecure,
                              const std::string &certfile, const std::string &keyfile) {
            brain_sinks_.push_back(std::make_unique<WsPublishSink>(
                ioc_, host, port, path, insecure,
                to_string(rt_.venue), cfg_.symbol, certfile, keyfile));
            spdlog::info("[GFH] brain publish enabled: {}:{}{}", host, port, path);
        };
        if (!cfg_.brain_ws_host.empty())
            add_sink_(cfg_.brain_ws_host,  cfg_.brain_ws_port,  cfg_.brain_ws_path,
                      cfg_.brain_ws_insecure,  cfg_.brain_ws_certfile,  cfg_.brain_ws_keyfile);
        if (!cfg_.brain2_ws_host.empty())
            add_sink_(cfg_.brain2_ws_host, cfg_.brain2_ws_port, cfg_.brain2_ws_path,
                      cfg_.brain2_ws_insecure, cfg_.brain2_ws_certfile, cfg_.brain2_ws_keyfile);

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

        for (auto &s : brain_sinks_) s->start();

        if (rt_.caps.requires_ws_bootstrap)
        {
            set_state_(FeedSyncState::BOOTSTRAPPING, "venue_bootstrap_required");
            bootstrapWS();
            return FeedOpResult::OK;
        }

        arm_heartbeat_();
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
        for (auto &s : brain_sinks_) s->stop();
        disarm_ws_watchdog_();
        heartbeat_timer_.cancel();

        set_state_(FeedSyncState::DISCONNECTED, "stop");
        buffer_.clear();

        if (controller_)
            controller_->resetBook();
        persist_.reset();
        brain_sinks_.clear();
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
        spdlog::info("[GFH] connecting websocket venue={} host={} port={} target={} ping_interval_ms={} stale_after_ms={}",
                     to_string(rt_.venue), rt_.ws.host, rt_.ws.port, rt_.ws.target,
                     rt_.ws_ping_interval_ms, rt_.ws_stale_after_ms);
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

        spdlog::info("[GFH] websocket connected venue={} host={} port={} target={}",
                     to_string(rt_.venue), rt_.ws.host, rt_.ws.port, rt_.ws.target);

        if (!rt_.wsSubscribeFrame.empty())
        {
            spdlog::info("[GFH] sending ws subscribe venue={} payload_bytes={}",
                         to_string(rt_.venue), rt_.wsSubscribeFrame.size());
            ws_->send_text(rt_.wsSubscribeFrame);
        }

        if (rt_.caps.sync_mode == SyncMode::RestAnchored)
        {
            set_state_(FeedSyncState::WAIT_REST_SNAPSHOT, "ws_open_rest_anchored");
            spdlog::info("[GFH] requesting REST snapshot venue={}", to_string(rt_.venue));
            requestSnapshot();
        }
        else
        {
            set_state_(FeedSyncState::WAIT_WS_SNAPSHOT, "ws_open_ws_authoritative");
            spdlog::info("[GFH] waiting for WS snapshot venue={}", to_string(rt_.venue));
        }
    }

    /**
     * - Async GET snapshot
     */
    void GenericFeedHandler::requestSnapshot()
    {
        set_state_(FeedSyncState::WAIT_REST_SNAPSHOT, "request_snapshot");
        spdlog::info("[GFH] requesting REST snapshot venue={} host={} port={} target={}",
                     to_string(rt_.venue), rt_.rest.host, rt_.rest.port, rt_.restSnapshotTarget);

        rest_request_start_ns_ = now_ns_(); // C4: track REST request latency

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
                                 spdlog::warn("[GFH] REST snapshot rate limited venue={} status={} retry_ms=750",
                                              to_string(rt_.venue), status);
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

        // C4: REST snapshot staleness diagnostics
        {
            const std::int64_t rest_latency_ms = (recv_ts_ns - rest_request_start_ns_) / 1'000'000LL;
            const std::size_t buffered = buffer_.size();
            spdlog::info("[GFH] received REST snapshot venue={} body_bytes={} rest_latency_ms={} buffered_incrementals={}",
                         to_string(rt_.venue), body.size(), rest_latency_ms, buffered);
            if (rest_latency_ms > static_cast<std::int64_t>(cfg_.rest_timeout_ms) * 80 / 100) {
                spdlog::warn("[GFH] REST snapshot near timeout venue={} latency_ms={}", to_string(rt_.venue), rest_latency_ms);
            }
            if (buffered > max_buffer_ / 2) {
                spdlog::warn("[GFH] REST snapshot buffer half-full, snapshot may be very stale venue={}", to_string(rt_.venue));
            }
        }

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

        const auto snap_action = controller_->onSnapshot(snap, kind);
        if (snap_action == OrderBookController::Action::NeedResync)
        {
            restartSync("rest_snapshot_rejected");
            return;
        }
        maybe_persist_book_("snapshot_applied");

        /// Baseline loaded. We are not necessarily synced yet (RestAnchored must bridge).
        set_state_(FeedSyncState::WAIT_BRIDGE, "snapshot_applied");

        /// Apply buffered incrementals after snapshot
        drainBufferedIncrementals();

        if (controller_->isSynced())
        {
            spdlog::info("[GFH] bridged (post-snapshot drain) -> SYNCED");
            set_state_(FeedSyncState::SYNCED, "post_snapshot_bridge_complete");
        }
        else
        {
            spdlog::debug("[GFH] still WAIT_BRIDGE after drain");
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
        ++ctr_msgs_received_;
        std::string_view msg{data, len};
        const std::int64_t recv_ts_ns = now_ns_();
        last_ws_message_ns_ = recv_ts_ns;
        arm_ws_watchdog_();

        const FeedSyncState state = state_.load(std::memory_order_relaxed);

        if (state == FeedSyncState::WAIT_REST_SNAPSHOT)
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

        if (state == FeedSyncState::WAIT_WS_SNAPSHOT)
        {
            // first, try snapshot
            GenericSnapshotFormat snap;
            const bool isSnap = std::visit([&](auto const &a)
                                           { return a.isSnapshot(msg) && a.parseWsSnapshot(msg, snap); }, adapter_);

            if (isSnap)
            {
                snap.ts_recv_ns = recv_ts_ns;
                persist_snapshot_(snap, "ws_snapshot");
                const auto snap_action = controller_->onSnapshot(
                    snap, OrderBookController::BaselineKind::WsAuthoritative);
                if (snap_action == OrderBookController::Action::NeedResync)
                {
                    restartSync("ws_snapshot_rejected");
                    return;
                }
                maybe_persist_book_("snapshot_applied");

                // baseline is WS snapshot; any buffered msgs were pre-baseline, drain them now
                state_.store(FeedSyncState::WAIT_BRIDGE, std::memory_order_relaxed);
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
        if ((state == FeedSyncState::WAIT_BRIDGE || state == FeedSyncState::SYNCED) && rt_.caps.ws_sends_snapshot)
        {
            GenericSnapshotFormat snap;
            const bool isSnap = std::visit([&](auto const &a)
                                           { return a.isSnapshot(msg) && a.parseWsSnapshot(msg, snap); }, adapter_);

            if (isSnap)
            {
                // Hard re-baseline (venue may resend snapshot on internal resync)
                snap.ts_recv_ns = recv_ts_ns;
                persist_snapshot_(snap, "ws_snapshot");
                const auto snap_action = controller_->onSnapshot(
                    snap, OrderBookController::BaselineKind::WsAuthoritative);
                if (snap_action == OrderBookController::Action::NeedResync)
                {
                    restartSync("interrupting_ws_snapshot_rejected");
                    return;
                }
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
        if (state == FeedSyncState::WAIT_BRIDGE || state == FeedSyncState::SYNCED)
        {
            // --- RestAnchored: during WAIT_BRIDGE we ONLY buffer+drain ---
            if (rt_.caps.sync_mode == SyncMode::RestAnchored && state == FeedSyncState::WAIT_BRIDGE)
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
                    spdlog::info("[GFH] bridged (ws buffered path) -> SYNCED");
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
            ++ctr_book_updates_;
            maybe_persist_book_("incremental_applied");

            if (state == FeedSyncState::WAIT_BRIDGE && controller_->isSynced())
            {
                spdlog::info("[GFH] bridged (ws path) -> SYNCED");
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

        ++ctr_resyncs_;
        const FeedSyncState cur_state = state_.load(std::memory_order_relaxed);
        if (reason.empty()) {
            spdlog::warn("[GFH] restart sync venue={} state={}",
                         to_string(rt_.venue), sync_state_to_string_(cur_state));
        } else {
            spdlog::warn("[GFH] restart sync venue={} state={} reason={}",
                         to_string(rt_.venue), sync_state_to_string_(cur_state), reason);
        }

        disarm_ws_watchdog_();
        buffer_.clear();
        controller_->resetBook();

        // Reset state before reconnect
        set_state_(FeedSyncState::CONNECTING, "restart_sync");

        // Correlate bootstrap if needed
        connect_id_ = makeConnectId();

        // Force-close current WS (immediate) and reconnect after exponential backoff.
        closing_for_restart_ = true;
        ws_->cancel();

        schedule_ws_reconnect_(next_reconnect_delay_());
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
            spdlog::error("[GFH] venue requires WS bootstrap but adapter did not provide target");
            restartSync("bootstrap_missing_target");
            return;
        }

        const std::string body = std::visit([&](auto const &a)
                                            { return a.wsBootstrapBody(cfg_); }, adapter_);

        spdlog::info("[GFH] requesting ws bootstrap venue={} host={} port={} target={} body_bytes={}",
                     to_string(rt_.venue), rt_.rest.host, rt_.rest.port, target, body.size());

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

                              spdlog::info("[GFH] ws bootstrap resolved venue={} host={} port={} target={} ping_interval_ms={} ping_timeout_ms={}",
                                           to_string(rt_.venue), info.ws.host, info.ws.port, info.ws.target,
                                           info.ping_interval_ms, info.ping_timeout_ms);

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

        spdlog::info("[GFH] scheduling reconnect venue={} delay_ms={} next_mode={}",
                     to_string(rt_.venue), delay.count(),
                     (rt_.caps.requires_ws_bootstrap ? "bootstrap" : "connect"));

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
            spdlog::info("[GFH] feed watchdog active venue={} timeout_ms={}",
                         to_string(rt_.venue), rt_.ws_stale_after_ms);
            ws_watchdog_announced_ = true;
        }
        ws_watchdog_timer_.expires_after(std::chrono::milliseconds(rt_.ws_stale_after_ms));
        ws_watchdog_timer_.async_wait([this, my_gen](const boost::system::error_code &ec)
                                      {
            if (ec) return;
            if (!running_.load()) return;
            if (my_gen != ws_watchdog_gen_) return;
            const FeedSyncState wdog_state = state_.load(std::memory_order_relaxed);
            if (wdog_state == FeedSyncState::DISCONNECTED || wdog_state == FeedSyncState::CONNECTING || wdog_state == FeedSyncState::BOOTSTRAPPING) {
                return;
            }

            const std::int64_t now = now_ns_();
            const std::int64_t age_ns = now - last_ws_message_ns_;
            if (last_ws_message_ns_ > 0 && age_ns >= static_cast<std::int64_t>(rt_.ws_stale_after_ms) * 1'000'000LL) {
                spdlog::warn("[GFH] stale WS feed detected, restarting sync venue={} silence_ms={}",
                             to_string(rt_.venue), age_ns / 1'000'000LL);
                restartSync("stale_ws_feed");
            } });
    }

    void GenericFeedHandler::disarm_ws_watchdog_()
    {
        ++ws_watchdog_gen_;
        ws_watchdog_announced_ = false;
        ws_watchdog_timer_.cancel();
    }

    // B6: exponential backoff helpers

    std::chrono::milliseconds GenericFeedHandler::next_reconnect_delay_() noexcept {
        // Apply ±25 % jitter using a thread_local PRNG (no locks needed on single-threaded Asio)
        static thread_local std::mt19937 rng{std::random_device{}()};
        static thread_local std::uniform_real_distribution<double> jitter(-0.25, 0.25);

        const int base = reconnect_delay_ms_;
        const int jittered = static_cast<int>(base * (1.0 + jitter(rng)));

        // Advance backoff: double for next call, capped at max
        reconnect_delay_ms_ = std::min(reconnect_delay_ms_ * 2, kReconnectMaxMs);

        return std::chrono::milliseconds(std::max(100, jittered)); // floor at 100 ms
    }

    void GenericFeedHandler::reset_reconnect_delay_() noexcept {
        reconnect_delay_ms_ = kReconnectInitMs;
    }

    void GenericFeedHandler::persist_snapshot_(const GenericSnapshotFormat &snap, std::string_view source)
    {
        if (persist_) persist_->write_snapshot(snap, source);
        for (auto &s : brain_sinks_) s->publish_snapshot(snap, source);
    }

    void GenericFeedHandler::persist_incremental_(const GenericIncrementalFormat &inc, std::string_view source)
    {
        if (persist_) persist_->write_incremental(inc, source);
        for (auto &s : brain_sinks_) s->publish_incremental(inc, source);
    }

    void GenericFeedHandler::maybe_persist_book_(std::string_view source)
    {
        if ((!persist_ && brain_sinks_.empty()) || !controller_)
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
        for (auto &s : brain_sinks_)
            s->publish_book_state(controller_->book(),
                                  controller_->getAppliedSeqID(),
                                  persist_book_top_,
                                  source,
                                  ts_book_ns);
    }

    // -------------------------------------------------------------------------
    // D2: per-feed heartbeat

    void GenericFeedHandler::arm_heartbeat_()
    {
        constexpr int kIntervalSec = 60;
        heartbeat_timer_.expires_after(std::chrono::seconds(kIntervalSec));
        heartbeat_timer_.async_wait([this](const boost::system::error_code &ec)
        {
            if (ec || !running_.load()) return;
            emit_heartbeat_();
            arm_heartbeat_(); // re-arm
        });
    }

    void GenericFeedHandler::emit_heartbeat_()
    {
        constexpr int kIntervalSec = 60;
        const std::string venue = to_string(rt_.venue);
        const std::string state = sync_state_to_string_(state_.load(std::memory_order_relaxed));
        const std::uint64_t msgs_per_sec = ctr_msgs_received_ / kIntervalSec;
        spdlog::info("[HEARTBEAT] venue={} state={} msgs_received={} msgs_per_sec={} book_updates={} resyncs={}",
                     venue, state, ctr_msgs_received_, msgs_per_sec, ctr_book_updates_,
                     ctr_resyncs_.load(std::memory_order_relaxed));

        // C2: warn if rate exceeds 2× configured ceiling
        if (cfg_.max_msg_rate_per_sec > 0 &&
            msgs_per_sec > static_cast<std::uint64_t>(cfg_.max_msg_rate_per_sec) * 2) {
            spdlog::warn("[HEARTBEAT] msg rate {}/s exceeds 2x ceiling {}/s venue={}",
                         msgs_per_sec, cfg_.max_msg_rate_per_sec, venue);
        }
        // Reset per-interval counters; lifetime counters (resyncs) are kept
        ctr_msgs_received_ = 0;
        ctr_book_updates_  = 0;
    }
}
