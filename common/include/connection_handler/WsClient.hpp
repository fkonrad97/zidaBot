#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace md {
    class WsClient : public std::enable_shared_from_this<WsClient> {
    public:
        using RawMessageHandler = std::function<void(const char *, std::size_t)>;
        using CloseHandler = std::function<void()>;
        using OpenHandler = std::function<void()>;
        using LogFn = std::function<void(std::string_view)>;

        static std::shared_ptr<WsClient> create(boost::asio::io_context &ioc) {
            return std::shared_ptr<WsClient>(new WsClient(ioc));
        }

        WsClient(const WsClient &) = delete;

        WsClient &operator=(const WsClient &) = delete;

        void set_on_raw_message(RawMessageHandler h);

        void set_on_close(CloseHandler h);

        void set_on_open(OpenHandler h);

        void set_logger(LogFn fn) { logger_ = std::move(fn); }

        void set_connect_timeout(std::chrono::milliseconds t) { connect_timeout_ = t; }
        void set_idle_ping(std::chrono::milliseconds interval) { ping_interval_ = interval; }

        // TLS verification is enabled by default.
        // For local testing with self-signed certs you can disable it.
        void set_tls_verify_peer(bool enabled) { tls_verify_peer_ = enabled; }

        // F1: mTLS — load a client certificate and private key (PEM files).
        // Must be called before connect().  Throws boost::system::system_error on failure.
        void set_client_cert(const std::string &certfile, const std::string &keyfile);

        // Limit number of queued outgoing messages when the socket is not writable/open.
        // If exceeded, the oldest messages are dropped.
        void set_max_outbox(std::size_t max) { max_outbox_ = max; }

        void connect(std::string host, std::string port, std::string target);

        void send_text(std::string text);

        // H1: send a binary WebSocket frame (MessagePack payload).
        void send_binary(std::string data);

        void close();

        void cancel();

    private:
        explicit WsClient(boost::asio::io_context &ioc);

        void do_resolve_();

        void do_tcp_connect_(const boost::asio::ip::tcp::resolver::results_type &results);

        void do_tls_handshake_();

        void do_ws_handshake_();

        void do_read_();

        void start_write_();

        void do_write_();

        void arm_connect_deadline_();

        void disarm_connect_deadline_();

        void start_ping_loop_();

        void stop_ping_loop_();

        void fail_(boost::system::error_code ec, std::string_view where);

        void notify_close_once_();

        void close_socket_hard_() noexcept;

        void emit_log_(std::string_view msg) {
            if (logger_) logger_(msg);
        }

    private:
        using tcp = boost::asio::ip::tcp;

        using websocket_stream =
        boost::beast::websocket::stream<
            boost::beast::ssl_stream<tcp::socket> >;

        boost::asio::io_context &ioc_;
        boost::asio::strand<boost::asio::io_context::executor_type> strand_;

        boost::asio::ssl::context ssl_ctx_{boost::asio::ssl::context::tls_client};

        websocket_stream ws_;
        tcp::resolver resolver_;

        boost::beast::flat_buffer buffer_;
        std::string message_; // reused capacity for read payload

        std::string host_;
        std::string port_;
        std::string target_;

        bool closing_{false};
        std::atomic_bool close_notified_{false};
        bool opened_{false};

        // H1: per-message binary flag so text and binary frames can coexist.
        struct OutboxMsg { std::string data; bool binary{false}; };
        std::deque<OutboxMsg> outbox_;
        std::size_t max_outbox_{10'000};
        bool write_in_flight_{false};

        boost::asio::steady_timer connect_deadline_;
        std::chrono::milliseconds connect_timeout_{5000};

        boost::asio::steady_timer ping_timer_;
        std::chrono::milliseconds ping_interval_{0}; // 0 = disabled

        bool tls_verify_peer_{true};

        CloseHandler on_close_;
        RawMessageHandler on_raw_message_;
        OpenHandler on_open_;
        LogFn logger_;
    };
} // namespace md
