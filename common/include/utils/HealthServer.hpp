#pragma once

#include <functional>
#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <spdlog/spdlog.h>

namespace md {

/// Minimal plain-HTTP health endpoint.
///
/// Responds to any inbound HTTP request with a JSON body produced by the
/// provided callback. Shares the caller's io_context — no new threads.
/// Disabled silently when port == 0.
class HealthServer {
public:
    using JsonFn = std::function<std::string()>;

    HealthServer(boost::asio::io_context &ioc, std::uint16_t port, JsonFn fn)
        : port_(port), fn_(std::move(fn)), acceptor_(ioc) {}

    void start() {
        if (port_ == 0) return;
        using tcp = boost::asio::ip::tcp;
        boost::beast::error_code ec;
        const tcp::endpoint ep(tcp::v4(), port_);
        acceptor_.open(ep.protocol(), ec);
        if (ec) { spdlog::warn("[HealthServer] open: {}", ec.message()); return; }
        acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
        acceptor_.bind(ep, ec);
        if (ec) { spdlog::warn("[HealthServer] bind port {}: {}", port_, ec.message()); return; }
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) { spdlog::warn("[HealthServer] listen: {}", ec.message()); return; }
        spdlog::info("[HealthServer] listening on port {}", port_);
        do_accept_();
    }

    void stop() {
        boost::beast::error_code ec;
        acceptor_.close(ec);
    }

private:
    void do_accept_() {
        acceptor_.async_accept(
            [this](boost::beast::error_code ec, boost::asio::ip::tcp::socket sock) {
                if (ec) return; // acceptor closed or error — stop loop
                serve_(std::make_shared<boost::asio::ip::tcp::socket>(std::move(sock)));
                do_accept_();
            });
    }

    void serve_(std::shared_ptr<boost::asio::ip::tcp::socket> sock) {
        using namespace boost::beast;
        using namespace boost::beast::http;

        auto buf = std::make_shared<flat_buffer>();
        auto req = std::make_shared<request<string_body>>();
        async_read(*sock, *buf, *req,
            [this, sock, buf, req](error_code ec, std::size_t) {
                if (ec) return;
                const std::string body = fn_();
                response<string_body> res{status::ok, req->version()};
                res.set(field::content_type, "application/json");
                res.set(field::connection, "close");
                res.content_length(body.size());
                res.body() = body;
                res.prepare_payload();
                auto res_ptr = std::make_shared<response<string_body>>(std::move(res));
                async_write(*sock, *res_ptr,
                    [sock, res_ptr](error_code, std::size_t) {
                        error_code ignored;
                        sock->shutdown(
                            boost::asio::ip::tcp::socket::shutdown_both, ignored);
                    });
            });
    }

    std::uint16_t port_;
    JsonFn fn_;
    boost::asio::ip::tcp::acceptor acceptor_;
};

} // namespace md
