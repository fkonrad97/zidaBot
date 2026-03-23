// RestClient.hpp
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace md {

    // Custom error category for HTTP-level errors (non-2xx responses).
    // Allows callers to distinguish exchange HTTP errors (4xx, 5xx) from
    // transport/protocol failures (connection reset, TLS error, timeout).
    enum class http_errc { non_2xx = 1 };

    struct http_error_category_impl : boost::system::error_category {
        const char *name() const noexcept override { return "http"; }
        std::string message(int ev) const override {
            return (ev == static_cast<int>(http_errc::non_2xx))
                       ? "HTTP non-2xx response"
                       : "unknown http error";
        }
    };

    inline const boost::system::error_category &http_error_category() {
        static http_error_category_impl inst;
        return inst;
    }

    inline boost::system::error_code make_error_code(http_errc e) {
        return {static_cast<int>(e), http_error_category()};
    }

    class RestClient : public std::enable_shared_from_this<RestClient> {
    public:
        using ResponseHandler = std::function<void(boost::system::error_code, std::string)>;
        using LogFn = std::function<void(std::string_view)>;

        // Factory: ensures shared_from_this() is always valid.
        static std::shared_ptr<RestClient> create(boost::asio::io_context &ioc) {
            return std::shared_ptr<RestClient>(new RestClient(ioc));
        }

        // Non-copyable / non-movable (simplifies lifetime & correctness).
        RestClient(const RestClient &) = delete;

        RestClient &operator=(const RestClient &) = delete;

        // Optional: keep the TLS connection alive and reuse it for subsequent requests
        // to the same (host,port). Default = false to preserve your previous behavior.
        void set_keep_alive(bool enabled) { keep_alive_ = enabled; }

        // Optional: allow caller to inject logger.
        void set_logger(LogFn fn) { logger_ = std::move(fn); }

        void async_get(std::string host,
                       std::string port,
                       std::string target,
                       ResponseHandler cb);

        void async_post(std::string host,
                        std::string port,
                        std::string target,
                        std::string body,
                        ResponseHandler cb);

        int last_http_status() const noexcept { return last_http_status_; }

        void set_timeout(std::chrono::milliseconds t) { timeout_ = t; }
        void set_shutdown_timeout(std::chrono::milliseconds t) { shutdown_timeout_ = t; }

        /// expose limits if you want to tune per venue/depth
        void set_limits(std::size_t max_header_bytes, std::size_t max_body_bytes) {
            max_header_bytes_ = max_header_bytes;
            max_body_bytes_ = max_body_bytes;
        }

        /// Cancel any in-flight request and complete callback with operation_aborted.
        /// Safe to call from any thread.
        void cancel();

    private:
        explicit RestClient(boost::asio::io_context &ioc);

        // Chain helpers (all executed on strand_)
        void do_resolve_();

        void do_tcp_connect_(const boost::asio::ip::tcp::resolver::results_type &results);

        void do_tls_handshake_();

        void do_http_request_();

        void do_http_read_();

        void do_tls_shutdown_();

        void fail_(boost::system::error_code ec);

        void finish_();

        // state mgmt
        void reset_per_request_state_(bool allow_reuse);

        bool can_reuse_connection_() const;

        void close_socket_hard_() noexcept;

        // deadlines
        void arm_deadline_();

        void disarm_deadline_();

        void arm_shutdown_deadline_();

        void cancel_shutdown_deadline_();

        void emit_log_(std::string_view msg) {
            if (logger_) logger_(msg);
        }

    private:
        boost::asio::io_context &ioc_;

        // Serialize *all* operations.
        boost::asio::strand<boost::asio::io_context::executor_type> strand_;

        boost::asio::ssl::context ssl_ctx_{boost::asio::ssl::context::tls_client};
        boost::asio::ip::tcp::resolver resolver_;

        std::unique_ptr<boost::beast::ssl_stream<boost::asio::ip::tcp::socket> > stream_;

        boost::beast::flat_buffer buffer_;
        boost::beast::http::request<boost::beast::http::string_body> req_;
        std::unique_ptr<boost::beast::http::response_parser<boost::beast::http::string_body> > parser_;

        std::string host_;
        std::string target_;
        std::string port_;
        std::string post_body_;

        ResponseHandler cb_;

        std::atomic_bool in_flight_{false};
        bool shutting_down_{false};

        boost::system::error_code final_ec_;
        std::string response_body_;

        bool tcp_connected_{false};
        bool tls_handshook_{false};

        // for keep-alive reuse
        bool keep_alive_{false};
        bool server_keep_alive_{false};
        std::string connected_host_;
        std::string connected_port_;

        // bounded deadlines
        boost::asio::steady_timer deadline_;
        std::chrono::milliseconds timeout_{5000};

        boost::asio::steady_timer shutdown_deadline_;
        std::chrono::milliseconds shutdown_timeout_{200};

        // safety limits
        std::size_t max_header_bytes_{32 * 1024};
        std::size_t max_body_bytes_{2 * 1024 * 1024};

        int last_http_status_{0};

        LogFn logger_;
    };
} // namespace md
