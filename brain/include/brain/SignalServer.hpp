#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace brain
{

    class SignalSession;

    /// Outbound-only TLS WebSocket server. Pushes arb cross signals to exec
    /// subscribers. Contrast with WsServer (inbound): sessions here send frames,
    /// not receive them.
    class SignalServer
    {
    public:
        SignalServer(boost::asio::io_context &ioc,
                     boost::asio::ssl::context &ssl_ctx,
                     boost::asio::ip::tcp::endpoint endpoint);

        /// Start acception connections. Must be called before ioc.run().
        void start();

        /// Stop accepting and close all active sessions.
        void stop();

        /// Broadcast a text frame to all live sessions.
        /// Safe to call from any thread (e.g. the brain scan thread)
        void broadcast(std::string text);

        /// Count of currently live (non-expired) sessions.
        [[nodiscard]] std::size_t session_count() const noexcept;

    private:
        void do_accept_();
        void on_accept_(boost::beast::error_code ec,
                        boost::asio::ip::tcp::socket socket);

        boost::asio::io_context &ioc_;
        boost::asio::ssl::context &ssl_ctx_;
        boost::asio::ip::tcp::acceptor acceptor_;
        std::vector<std::weak_ptr<SignalSession>> sessions_;
        bool stopped_{false};
    };

    /// One per accepted exec connection. Lifecycle:
    ///     run() -> TLS handshake -> WS accept -> read loop
    ///     send() posts frames onto the session strand -> write loop
    class SignalSession : public std::enable_shared_from_this<SignalSession>
    {
    public:
        using WsStream = boost::beast::websocket::stream<boost::beast::ssl_stream<boost::asio::ip::tcp::socket>>;

        static std::shared_ptr<SignalSession> create(
            boost::asio::ip::tcp::socket socket,
            boost::asio::ssl::context &ssl_ctx);

        void run();
        void close();

        /// Enqueue a text frame for async delivery. Safe to call from any thread.
        void send(std::string msg);

    private:
        SignalSession(boost::asio::ip::tcp::socket socket,
                      boost::asio::ssl::context &ssl_ctx);

        void do_tls_handshake_();
        void do_ws_accept_();
        void do_read_();
        void do_write_();
        void on_write_(boost::beast::error_code ec);
        void fail_(boost::beast::error_code ec, std::string_view where);

        static constexpr std::size_t kMaxOutbox = 64;

        WsStream ws_;
        boost::beast::flat_buffer buffer_;
        std::deque<std::string> outbox_;
        bool writing_{false};
        std::string remote_addr_;
    };
} // namespace brain