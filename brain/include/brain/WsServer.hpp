#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace brain {

class WsSession;

/// Async TLS WebSocket server. Accepts inbound connections from PoP
/// WsPublishSink clients and invokes on_message for each received text frame.
class WsServer {
public:
    // H1: second argument is true when the frame is a binary (MessagePack) frame.
    using MessageHandler = std::function<void(std::string_view, bool is_binary)>;

    WsServer(boost::asio::io_context &ioc,
             boost::asio::ssl::context &ssl_ctx,
             boost::asio::ip::tcp::endpoint endpoint,
             MessageHandler on_message);

    /// Start accepting connections. Must be called before ioc.run().
    void start();

    /// Stop accepting and close all active sessions.
    void stop();

    /// Count of currently live (non-expired) sessions.
    [[nodiscard]] std::size_t session_count() const noexcept {
        return static_cast<std::size_t>(
            std::count_if(sessions_.begin(), sessions_.end(),
                          [](const std::weak_ptr<WsSession> &w) { return !w.expired(); }));
    }

private:
    void do_accept_();
    void on_accept_(boost::beast::error_code ec,
                    boost::asio::ip::tcp::socket socket);

    boost::asio::io_context          &ioc_;
    boost::asio::ssl::context        &ssl_ctx_;
    boost::asio::ip::tcp::acceptor    acceptor_;
    MessageHandler                    on_message_;
    std::vector<std::weak_ptr<WsSession>> sessions_;
    bool stopped_{false};
};

// ---------------------------------------------------------------------------

/// One per accepted connection. Lifecycle:
///   run() → TLS handshake → WS accept → read loop
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    using WsStream = boost::beast::websocket::stream<
                         boost::beast::ssl_stream<boost::asio::ip::tcp::socket>>;

    static std::shared_ptr<WsSession> create(
        boost::asio::ip::tcp::socket      socket,
        boost::asio::ssl::context        &ssl_ctx,
        WsServer::MessageHandler          on_message);

    void run();
    void close();

private:
    WsSession(boost::asio::ip::tcp::socket      socket,
              boost::asio::ssl::context        &ssl_ctx,
              WsServer::MessageHandler          on_message);

    void do_tls_handshake_();
    void do_ws_accept_();
    void do_read_();
    void fail_(boost::beast::error_code ec, std::string_view where);

    WsStream                    ws_;
    boost::beast::flat_buffer   buffer_;
    WsServer::MessageHandler    on_message_;
    std::string                 remote_addr_;
};

} // namespace brain
