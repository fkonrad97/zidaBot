#include "brain/SignalServer.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/buffers_to_string.hpp>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

namespace brain
{

    // ---------------------------------------------------------------------------
    // SignalServer

    // Performs the standard acceptor setup: open → reuse_address → bind → listen.
    // Errors at any step are logged and the constructor returns early — start()
    // will silently no-op if the acceptor never reached the listen state.
    SignalServer::SignalServer(net::io_context &ioc,
                               ssl::context &ssl_ctx,
                               tcp::endpoint endpoint)
        : ioc_(ioc),
          ssl_ctx_(ssl_ctx),
          acceptor_(ioc), // bind to io_context directly
          strand_(net::make_strand(ioc))
    {
        beast::error_code ec;

        acceptor_.open(endpoint.protocol(), ec);
        if (ec)
        {
            spdlog::error("[SignalServer] open: {}", ec.message());
            return;
        }

        // Allow immediate rebind after restart without waiting for TIME_WAIT to expire.
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec)
        {
            spdlog::error("[SignalServer] set_option: {}", ec.message());
            return;
        }

        acceptor_.bind(endpoint, ec);
        if (ec)
        {
            spdlog::error("[SignalServer] bind: {}", ec.message());
            return;
        }

        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec)
        {
            spdlog::error("[SignalServer] listen: {}", ec.message());
            return;
        }

        spdlog::info("[SignalServer] listening on {}:{}",
                     endpoint.address().to_string(), endpoint.port());
    }

    // Posts the first async_accept onto the ioc. The accept loop is then
    // self-sustaining: on_accept_() re-arms do_accept_() after each connection.
    // Must be called before ioc.run() so the ioc has pending work to process.
    void SignalServer::start()
    {
        do_accept_();
    }

    // Sets stopped_ so do_accept_() will not re-arm, closes the acceptor to
    // unblock any in-flight async_accept, then gracefully closes all live sessions.
    // Dead weak_ptrs are pruned first to avoid calling lock() on already-expired entries.
    void SignalServer::stop()
    {
        stopped_.store(true, std::memory_order_relaxed);
        beast::error_code ec;
        acceptor_.close(ec);

        net::dispatch(strand_, [self = shared_from_this()]()
        {
            self->sessions_.erase(
                std::remove_if(self->sessions_.begin(), self->sessions_.end(),
                               [](const std::weak_ptr<SignalSession> &w)
                               { return w.expired(); }),
                self->sessions_.end());

            for (auto &wptr : self->sessions_)
            {
                if (auto sp = wptr.lock())
                    sp->close();
            }
            self->sessions_.clear();
        });
    }

    // Dispatches onto strand_ to take a thread-safe snapshot of live sessions,
    // then calls send() on each live session outside the lock. send() posts onto
    // the session's own strand so this method is safe to call from any thread
    // (e.g. the scan thread via the on_cross_ callback).
    void SignalServer::broadcast(std::string text)
    {
        net::dispatch(strand_, [self = shared_from_this(), text = std::move(text)]() mutable
        {
            // Prune expired weak_ptrs — runs on strand_ so sessions_ access is safe.
            self->sessions_.erase(
                std::remove_if(self->sessions_.begin(), self->sessions_.end(),
                               [](const std::weak_ptr<SignalSession> &w)
                               { return w.expired(); }),
                self->sessions_.end());

            // Take a snapshot of live shared_ptrs, then send outside the strand
            // so a slow session cannot block others.
            std::vector<std::shared_ptr<SignalSession>> live;
            live.reserve(self->sessions_.size());
            for (auto &wptr : self->sessions_)
            {
                if (auto sp = wptr.lock())
                    live.push_back(sp);
            }

            for (auto &sp : live)
                sp->send(text);
        });
    }

    // Counts non-expired weak_ptrs without touching session internals.
    // Used by brain's health endpoint to report connected exec clients.
    // Note: called from the I/O thread; safe because sessions_ is only mutated
    // from strand_ and this is a best-effort read for monitoring purposes.
    std::size_t SignalServer::session_count() const noexcept
    {
        return static_cast<std::size_t>(
            std::count_if(sessions_.begin(), sessions_.end(),
                          [](const std::weak_ptr<SignalSession> &w)
                          { return !w.expired(); }));
    }

    // Arms a single async_accept. The accepted socket gets its own strand
    // (net::make_strand(ioc_)) which becomes the SignalSession's executor —
    // all session state (outbox_, writing_) must only be touched from that strand.
    // do_accept_ and on_accept_ themselves run on strand_ (server-level strand).
    void SignalServer::do_accept_()
    {
        if (stopped_.load(std::memory_order_relaxed))
            return;

        acceptor_.async_accept(
            strand_,
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket)
            {
                self->on_accept_(ec, std::move(socket));
            });
    }

    // Handles a completed async_accept. Prunes dead sessions, enforces the
    // kMaxSessions cap (drops the new socket if full), creates and starts the
    // session, then immediately re-arms the next async_accept.
    void SignalServer::on_accept_(beast::error_code ec, tcp::socket socket)
    {
        if (ec)
        {
            // operation_aborted is expected during stop() — not a real error.
            if (ec != net::error::operation_aborted)
                spdlog::warn("[SignalServer] accept error: {}", ec.message());
            return;
        }

        // Lazy prune: remove sessions whose shared_ptr has already been destroyed.
        sessions_.erase(
            std::remove_if(sessions_.begin(), sessions_.end(),
                           [](const std::weak_ptr<SignalSession> &w)
                           { return w.expired(); }),
            sessions_.end());

        // Hard cap: one exec process per venue (5 venues) plus headroom.
        // Excess connections are dropped immediately after re-arming the acceptor.
        constexpr std::size_t kMaxSessions = 10;
        if (sessions_.size() >= kMaxSessions)
        {
            spdlog::warn("[SignalServer] connection limit reached ({}), dropping new client", kMaxSessions);
            do_accept_();
            return;
        }

        // Give the session socket its own per-session strand, distinct from the
        // server-level strand_. This serialises all session async ops independently
        // so a slow session never blocks others or the accept loop.
        auto session = SignalSession::create(
            tcp::socket(net::make_strand(ioc_), socket.local_endpoint().protocol(),
                        socket.release()),
            ssl_ctx_);
        sessions_.emplace_back(session);
        session->run();

        // Re-arm immediately so the next exec client can connect without delay.
        do_accept_();
    }

    // ---------------------------------------------------------------------------
    // SignalSession

    // Moves the raw socket into the three-layer ws_ stack (tcp → ssl → websocket).
    // Captures remote_addr_ immediately while the socket is still open;
    // falls back to "<unknown>" if the endpoint is unavailable.
    SignalSession::SignalSession(tcp::socket socket,
                                 ssl::context &ssl_ctx)
        : ws_(std::move(socket), ssl_ctx)
    {
        try
        {
            // Two next_layer() calls: peel off websocket → peel off ssl → reach tcp socket.
            remote_addr_ = ws_.next_layer().next_layer().remote_endpoint().address().to_string();
        }
        catch (...)
        {
            remote_addr_ = "<unknown>";
        }
    }

    // Private constructor enforces heap allocation. make_shared cannot be used
    // because the constructor is private — shared_from_this() requires the object
    // to already be managed by a shared_ptr before it is called.
    std::shared_ptr<SignalSession> SignalSession::create(
        tcp::socket socket,
        ssl::context &ssl_ctx)
    {
        return std::shared_ptr<SignalSession>(
            new SignalSession(std::move(socket), ssl_ctx));
    }

    // Dispatches onto the session's strand before starting the handshake.
    // Guarantees all subsequent async operations run serialised on the strand
    // even if run() is called from outside the ioc thread.
    void SignalSession::run()
    {
        net::dispatch(ws_.get_executor(), [self = shared_from_this()]
                      { self->do_tls_handshake_(); });
    }

    // Forcibly closes the underlying TCP socket. Dispatches onto the session's
    // strand so the close does not race with in-flight async_read/async_write ops.
    // Unblocking those ops causes them to complete with an error and allows the
    // session's shared_ptr refcount to reach zero naturally.
    void SignalSession::close()
    {
        net::post(ws_.get_executor(), [self = shared_from_this()]
        {
            beast::error_code ec;
            self->ws_.next_layer().next_layer().close(ec);
        });
    }

    // Called from any thread (typically the brain scan thread via on_cross_).
    // Posts a lambda onto the session's strand so outbox_ and writing_ are
    // only ever touched from the ioc thread — no mutex needed.
    // Drop-oldest policy: if outbox_ is full, the stale front entry is evicted
    // before the new message is pushed. Signals are time-sensitive so old ones
    // are worthless if exec has fallen behind.
    void SignalSession::send(std::string msg)
    {
        net::post(ws_.get_executor(),
                  [self = shared_from_this(), msg = std::move(msg)]() mutable
                  {
                      if (self->outbox_.size() >= kMaxOutbox)
                          self->outbox_.pop_front(); // evict oldest, keep newest
                      self->outbox_.push_back(std::move(msg));
                      // Only kick off do_write_() if no write is already in flight.
                      // If writing_ is true, on_write_() will drain the queue itself.
                      if (!self->writing_)
                          self->do_write_();
                  });
    }

    // Performs the TLS server handshake on the ssl_stream layer (next_layer()).
    // On success, upgrades to WebSocket via do_ws_accept_().
    void SignalSession::do_tls_handshake_()
    {
        ws_.next_layer().async_handshake(
            ssl::stream_base::server,
            [self = shared_from_this()](beast::error_code ec)
            {
                if (ec)
                {
                    self->fail_(ec, "tls_handshake");
                    return;
                }
                self->do_ws_accept_();
            });
    }

    // Completes the WebSocket upgrade handshake. Sets the Server header to
    // "brain-signal/1.0" to distinguish this port from the inbound WsServer
    // ("brain/1.0"). Caps inbound frame size at 64 KiB (exec sends nothing
    // meaningful, so this is purely a defence against abuse). On success,
    // starts the read loop to detect disconnects.
    void SignalSession::do_ws_accept_()
    {
        ws_.read_message_max(65536);

        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type &res)
            {
                res.set(boost::beast::http::field::server, "brain-signal/1.0");
            }));

        ws_.async_accept(
            [self = shared_from_this()](beast::error_code ec)
            {
                if (ec)
                {
                    self->fail_(ec, "ws_accept");
                    return;
                }
                spdlog::info("[SignalServer] exec connected addr={}", self->remote_addr_);
                self->do_read_();
            });
    }

    // Read loop that keeps the connection alive and detects exec disconnects.
    // exec sends no meaningful data — every frame is discarded immediately.
    // closed / operation_aborted are clean shutdown codes, not errors.
    void SignalSession::do_read_()
    {
        ws_.async_read(
            buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t /*bytes*/)
            {
                if (ec)
                {
                    if (ec != websocket::error::closed &&
                        ec != net::error::operation_aborted)
                        self->fail_(ec, "read");
                    else
                        spdlog::info("[SignalServer] exec disconnected addr={}", self->remote_addr_);
                    return;
                }
                self->buffer_.consume(self->buffer_.size()); // discard, exec sends nothing
                self->do_read_();
            });
    }

    // Starts an async_write for the front of outbox_. Sets writing_ = true to
    // prevent send() from starting a second parallel write. ws_.text(true) marks
    // the frame as a text frame (JSON). The front entry is not popped until
    // on_write_() confirms delivery to avoid losing the message on error.
    void SignalSession::do_write_()
    {
        writing_ = true;
        ws_.text(true);
        ws_.async_write(
            net::buffer(outbox_.front()),
            [self = shared_from_this()](beast::error_code ec, std::size_t /*bytes*/)
            {
                self->on_write_(ec);
            });
    }

    // Completion handler for async_write. On success: pop the sent entry, chain
    // into the next do_write_() if more items are queued, otherwise go idle
    // (writing_ = false) until the next send() kick-starts the loop.
    void SignalSession::on_write_(beast::error_code ec)
    {
        if (ec)
        {
            fail_(ec, "write");
            writing_ = false; // allow future send() to retry
            return;
        }
        outbox_.pop_front();
        if (!outbox_.empty())
            do_write_();
        else
            writing_ = false;
    }

    // Logs the error with context. All async handlers funnel failures here
    // so the log format is consistent across the session lifecycle.
    void SignalSession::fail_(beast::error_code ec, std::string_view where)
    {
        spdlog::warn("[SignalServer] {}: {} addr={}", where, ec.message(), remote_addr_);
    }
} // namespace brain