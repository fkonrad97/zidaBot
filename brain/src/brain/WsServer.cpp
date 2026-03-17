#include "brain/WsServer.hpp"

#include <algorithm>
#include <iostream>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/buffers_to_string.hpp>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = boost::asio::ssl;
using tcp           = net::ip::tcp;

namespace brain {

// ---------------------------------------------------------------------------
// WsServer

WsServer::WsServer(net::io_context &ioc,
                   ssl::context    &ssl_ctx,
                   tcp::endpoint    endpoint,
                   MessageHandler   on_message)
    : ioc_(ioc),
      ssl_ctx_(ssl_ctx),
      acceptor_(ioc),           // bind to io_context directly
      on_message_(std::move(on_message)) {
    beast::error_code ec;

    acceptor_.open(endpoint.protocol(), ec);
    if (ec) { std::cerr << "[WsServer] open: " << ec.message() << "\n"; return; }

    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if (ec) { std::cerr << "[WsServer] set_option: " << ec.message() << "\n"; return; }

    acceptor_.bind(endpoint, ec);
    if (ec) { std::cerr << "[WsServer] bind: " << ec.message() << "\n"; return; }

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) { std::cerr << "[WsServer] listen: " << ec.message() << "\n"; return; }

    std::cerr << "[WsServer] listening on "
              << endpoint.address().to_string() << ":" << endpoint.port() << "\n";
}

void WsServer::start() {
    do_accept_();
}

void WsServer::stop() {
    stopped_ = true;
    beast::error_code ec;
    acceptor_.close(ec);

    // Purge dead entries before iterating
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
                       [](const std::weak_ptr<WsSession> &w) { return w.expired(); }),
        sessions_.end());

    for (auto &wptr : sessions_) {
        if (auto sp = wptr.lock())
            sp->close();
    }
    sessions_.clear();
}

void WsServer::do_accept_() {
    if (stopped_) return;

    // Pass a strand executor so each new session socket runs on its own strand
    acceptor_.async_accept(
        net::make_strand(ioc_),
        [self = this](beast::error_code ec, tcp::socket socket) {
            self->on_accept_(ec, std::move(socket));
        }
    );
}

void WsServer::on_accept_(beast::error_code ec, tcp::socket socket) {
    if (ec) {
        if (ec != net::error::operation_aborted)
            std::cerr << "[WsServer] accept error: " << ec.message() << "\n";
        return;
    }

    // Prune expired weak_ptrs (lazy cleanup)
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
                       [](const std::weak_ptr<WsSession> &w) { return w.expired(); }),
        sessions_.end());

    constexpr std::size_t kMaxSessions = 10;
    if (sessions_.size() >= kMaxSessions) {
        std::cerr << "[WsServer] connection limit reached (" << kMaxSessions << "), dropping new client\n";
        do_accept_();
        return;
    }

    auto session = WsSession::create(std::move(socket), ssl_ctx_, on_message_);
    sessions_.emplace_back(session);
    session->run();

    do_accept_();
}

// ---------------------------------------------------------------------------
// WsSession

WsSession::WsSession(tcp::socket      socket,
                     ssl::context    &ssl_ctx,
                     WsServer::MessageHandler on_message)
    : ws_(std::move(socket), ssl_ctx),
      on_message_(std::move(on_message)) {
    try {
        remote_addr_ = ws_.next_layer().next_layer().remote_endpoint()
                          .address().to_string();
    } catch (...) {
        remote_addr_ = "<unknown>";
    }
}

std::shared_ptr<WsSession> WsSession::create(
    tcp::socket              socket,
    ssl::context            &ssl_ctx,
    WsServer::MessageHandler on_message)
{
    return std::shared_ptr<WsSession>(
        new WsSession(std::move(socket), ssl_ctx, std::move(on_message)));
}

void WsSession::run() {
    // Dispatch onto the session's strand (inherited from the socket)
    net::dispatch(ws_.get_executor(), [self = shared_from_this()] {
        self->do_tls_handshake_();
    });
}

void WsSession::close() {
    beast::error_code ec;
    ws_.next_layer().next_layer().close(ec);
}

void WsSession::do_tls_handshake_() {
    ws_.next_layer().async_handshake(
        ssl::stream_base::server,
        [self = shared_from_this()](beast::error_code ec) {
            if (ec) { self->fail_(ec, "tls_handshake"); return; }
            self->do_ws_accept_();
        }
    );
}

void WsSession::do_ws_accept_() {
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type &res) {
            res.set(boost::beast::http::field::server, "brain/1.0");
        }
    ));

    ws_.async_accept(
        [self = shared_from_this()](beast::error_code ec) {
            if (ec) { self->fail_(ec, "ws_accept"); return; }
            std::cerr << "[WsServer] client connected addr=" << self->remote_addr_ << "\n";
            self->ws_.read_message_max(4 * 1024 * 1024); // 4 MB hard limit per frame
            self->do_read_();
        }
    );
}

void WsSession::do_read_() {
    ws_.async_read(
        buffer_,
        [self = shared_from_this()](beast::error_code ec, std::size_t /*bytes*/) {
            if (ec) {
                if (ec != websocket::error::closed &&
                    ec != net::error::operation_aborted)
                    self->fail_(ec, "read");
                else
                    std::cerr << "[WsServer] client disconnected addr="
                              << self->remote_addr_ << "\n";
                return;
            }

            if (self->ws_.got_text()) {
                const std::string data = beast::buffers_to_string(self->buffer_.data());
                try {
                    self->on_message_(std::string_view{data});
                } catch (...) {}
            }

            self->buffer_.consume(self->buffer_.size());
            self->do_read_();
        }
    );
}

void WsSession::fail_(beast::error_code ec, std::string_view where) {
    std::cerr << "[WsServer] " << where << ": " << ec.message()
              << " addr=" << remote_addr_ << "\n";
}

} // namespace brain
