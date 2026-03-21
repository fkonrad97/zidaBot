// RestClient.cpp
#include "connection_handler/RestClient.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/version.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h> // X509_check_host

namespace md {
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace ssl = boost::asio::ssl;
    using tcp = boost::asio::ip::tcp;

    RestClient::RestClient(boost::asio::io_context &ioc)
        : ioc_(ioc),
          strand_(ioc.get_executor()),
          ssl_ctx_(ssl::context::tls_client),
          resolver_(ioc_),
          deadline_(ioc_),
          shutdown_deadline_(ioc_) {
        // Baseline secure defaults
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(ssl::verify_peer);

        // Create initial stream (may be recreated per request if not reusing)
        stream_ = std::make_unique<beast::ssl_stream<tcp::socket> >(ioc_, ssl_ctx_);
        stream_->set_verify_mode(ssl::verify_peer);
    }

    bool RestClient::can_reuse_connection_() const {
        if (!keep_alive_) return false;
        if (!stream_) return false;
        if (!tcp_connected_ || !tls_handshook_) return false;
        if (host_ != connected_host_ || port_ != connected_port_) return false;

        const auto &sock = beast::get_lowest_layer(*stream_);
        return sock.is_open();
    }

    void RestClient::close_socket_hard_() noexcept {
        boost::system::error_code ignored;
        if (stream_) {
            auto &sock = beast::get_lowest_layer(*stream_);
            sock.cancel(ignored);
            sock.shutdown(tcp::socket::shutdown_both, ignored);
            sock.close(ignored);
        }
        tcp_connected_ = false;
        tls_handshook_ = false;
        connected_host_.clear();
        connected_port_.clear();
    }

    void RestClient::reset_per_request_state_(bool allow_reuse) {
        disarm_deadline_();
        cancel_shutdown_deadline_();

        shutting_down_ = false;
        final_ec_.clear();
        response_body_.clear();
        last_http_status_ = 0;
        server_keep_alive_ = false;

        buffer_.consume(buffer_.size());
        req_ = {};
        parser_ = std::make_unique<http::response_parser<http::string_body> >();
        parser_->header_limit(max_header_bytes_);
        parser_->body_limit(max_body_bytes_);

        // If we can reuse, do NOT recreate stream / verify callback.
        if (allow_reuse && can_reuse_connection_()) {
            return;
        }

        // Otherwise, rebuild stream and verification for current host_.
        close_socket_hard_();

        stream_ = std::make_unique<beast::ssl_stream<tcp::socket> >(ioc_, ssl_ctx_);
        stream_->set_verify_mode(ssl::verify_peer);

        // Hostname verification on leaf cert (depth 0)
        const std::string host_for_verify = host_;
        stream_->set_verify_callback(
            [host_for_verify](bool preverified, ssl::verify_context &ctx) {
                if (!preverified) return false;

                X509_STORE_CTX *sctx = ctx.native_handle();
                if (X509_STORE_CTX_get_error_depth(sctx) != 0) return true;

                X509 *cert = X509_STORE_CTX_get_current_cert(sctx);
                if (!cert) return false;

                return X509_check_host(
                           cert,
                           host_for_verify.c_str(),
                           host_for_verify.size(),
                           0,
                           nullptr) == 1;
            }
        );
    }

    void RestClient::async_get(std::string host, std::string port, std::string target, ResponseHandler cb) {
        auto self = shared_from_this();
        boost::asio::dispatch(strand_, [self,
                                  host = std::move(host),
                                  port = std::move(port),
                                  target = std::move(target),
                                  cb = std::move(cb)]() mutable {
                                  if (self->in_flight_.exchange(true)) {
                                      if (cb) {
                                          try {
                                              cb(make_error_code(boost::system::errc::operation_in_progress), {});
                                          } catch (...) {
                                          }
                                      }
                                      return;
                                  }

                                  self->host_ = std::move(host);
                                  self->port_ = std::move(port);
                                  self->target_ = std::move(target);
                                  self->post_body_.clear();
                                  self->cb_ = std::move(cb);

                                  if (self->target_.empty() || self->target_[0] != '/') {
                                      self->final_ec_ = make_error_code(boost::system::errc::invalid_argument);
                                      self->finish_();
                                      return;
                                  }

                                  self->reset_per_request_state_(/*allow_reuse*/true);
                                  self->arm_deadline_();

                                  self->req_.version(11);
                                  self->req_.method(http::verb::get);
                                  self->req_.target(self->target_);

                                  std::string host_hdr = self->host_;
                                  if (self->port_ != "443" && self->port_ != "80") host_hdr += ":" + self->port_;

                                  self->req_.set(http::field::host, host_hdr);
                                  self->req_.set(http::field::accept_encoding, "identity");
                                  self->req_.set(http::field::user_agent,
                                                 std::string(BOOST_BEAST_VERSION_STRING) + " pop-restclient");
                                  self->req_.set(http::field::connection, self->keep_alive_ ? "keep-alive" : "close");

                                  if (self->can_reuse_connection_()) {
                                      self->do_http_request_();
                                  } else {
                                      self->do_resolve_();
                                  }
                              });
    }

    void RestClient::async_post(std::string host, std::string port, std::string target, std::string body,
                                ResponseHandler cb) {
        auto self = shared_from_this();
        boost::asio::dispatch(strand_, [self,
                                  host = std::move(host),
                                  port = std::move(port),
                                  target = std::move(target),
                                  body = std::move(body),
                                  cb = std::move(cb)]() mutable {
                                  if (self->in_flight_.exchange(true)) {
                                      if (cb) {
                                          try {
                                              cb(make_error_code(boost::system::errc::operation_in_progress), {});
                                          } catch (...) {
                                          }
                                      }
                                      return;
                                  }

                                  self->host_ = std::move(host);
                                  self->port_ = std::move(port);
                                  self->target_ = std::move(target);
                                  self->post_body_ = std::move(body);
                                  self->cb_ = std::move(cb);

                                  if (self->target_.empty() || self->target_[0] != '/') {
                                      self->final_ec_ = make_error_code(boost::system::errc::invalid_argument);
                                      self->finish_();
                                      return;
                                  }

                                  self->reset_per_request_state_(/*allow_reuse*/true);
                                  self->arm_deadline_();

                                  self->req_.version(11);
                                  self->req_.method(http::verb::post);
                                  self->req_.target(self->target_);

                                  std::string host_hdr = self->host_;
                                  if (self->port_ != "443" && self->port_ != "80") host_hdr += ":" + self->port_;

                                  self->req_.set(http::field::host, host_hdr);
                                  self->req_.set(http::field::user_agent, "pop-restclient");
                                  self->req_.set(http::field::content_type, "application/json");
                                  self->req_.set(http::field::accept_encoding, "identity");
                                  self->req_.set(http::field::connection, self->keep_alive_ ? "keep-alive" : "close");

                                  self->req_.body() = std::move(self->post_body_);
                                  self->req_.prepare_payload();

                                  if (self->can_reuse_connection_()) {
                                      self->do_http_request_();
                                  } else {
                                      self->do_resolve_();
                                  }
                              });
    }

    void RestClient::fail_(boost::system::error_code ec) {
        // strand-only
        if (!in_flight_.load(std::memory_order_acquire)) return;
        if (final_ec_) return;
        final_ec_ = ec;

        // If we never established TLS, hard-close and finish immediately.
        if (!tcp_connected_ || !tls_handshook_) {
            close_socket_hard_();
            finish_();
            return;
        }

        // Established TLS -> attempt orderly shutdown unless keep-alive path (we still close on error).
        do_tls_shutdown_();
    }

    void RestClient::finish_() {
        // strand-only, ensure single completion
        if (!in_flight_.exchange(false)) return;

        disarm_deadline_();
        cancel_shutdown_deadline_();

        auto cb = std::move(cb_);
        cb_ = {};

        const auto ec = final_ec_;
        auto body = std::move(response_body_);

        // If we are not keeping the connection alive (or server did not agree), ensure socket is closed.
        if (!(keep_alive_ && server_keep_alive_ && !ec)) {
            close_socket_hard_();
        }

        if (cb) {
            try { cb(ec, std::move(body)); } catch (...) {
                // Never allow user callback to unwind through Asio.
                // Intentionally swallowed; inject logger if you want visibility.
            }
        }
    }

    void RestClient::do_resolve_() {
        auto self = shared_from_this();
        resolver_.async_resolve(
            host_, port_,
            boost::asio::bind_executor(strand_,
                                       [self](const boost::system::error_code &ec,
                                              const tcp::resolver::results_type &results) {
                                           if (ec) return self->fail_(ec);
                                           self->do_tcp_connect_(results);
                                       }
            )
        );
    }

    void RestClient::do_tcp_connect_(const tcp::resolver::results_type &results) {
        auto self = shared_from_this();

        boost::asio::async_connect(
            beast::get_lowest_layer(*stream_), results,
            boost::asio::bind_executor(strand_,
                                       [self](const boost::system::error_code &ec, const tcp::endpoint &) {
                                           if (ec) return self->fail_(ec);

                                           // SNI
                                           if (!SSL_set_tlsext_host_name(
                                               self->stream_->native_handle(), self->host_.c_str())) {
                                               const boost::system::error_code sni_ec{
                                                   static_cast<int>(::ERR_get_error()),
                                                   boost::asio::error::get_ssl_category()
                                               };
                                               return self->fail_(sni_ec);
                                           }

                                           self->tcp_connected_ = true;
                                           self->do_tls_handshake_();
                                       }
            )
        );
    }

    void RestClient::do_tls_handshake_() {
        auto self = shared_from_this();
        stream_->async_handshake(
            ssl::stream_base::client,
            boost::asio::bind_executor(strand_,
                                       [self](const boost::system::error_code &ec) {
                                           if (ec) return self->fail_(ec);

                                           self->tls_handshook_ = true;
                                           self->connected_host_ = self->host_;
                                           self->connected_port_ = self->port_;

                                           self->do_http_request_();
                                       }
            )
        );
    }

    void RestClient::do_http_request_() {
        auto self = shared_from_this();
        http::async_write(
            *stream_, req_,
            boost::asio::bind_executor(strand_,
                                       [self](const boost::system::error_code &ec, std::size_t) {
                                           if (ec) return self->fail_(ec);
                                           self->do_http_read_();
                                       }
            )
        );
    }

    void RestClient::do_http_read_() {
        auto self = shared_from_this();

        http::async_read(
            *stream_, buffer_, *parser_,
            boost::asio::bind_executor(strand_,
                                       [self](const boost::system::error_code &ec, std::size_t) {
                                           if (ec) return self->fail_(ec);

                                           self->disarm_deadline_();

                                           auto &res = self->parser_->get();
                                           self->last_http_status_ = res.result_int();
                                           self->server_keep_alive_ = res.keep_alive();
                                           self->response_body_ = std::move(res.body());

                                           self->final_ec_.clear();
                                           if (self->last_http_status_ < 200 || self->last_http_status_ >= 300) {
                                               // Preserve body (venues often put details there).
                                               // Use http_errc::non_2xx so callers can distinguish an
                                               // exchange-level error from a transport/TLS failure.
                                               self->final_ec_ = make_error_code(http_errc::non_2xx);
                                           }

                                           // If keep-alive is enabled AND server agrees AND no error, do not TLS-shutdown.
                                           if (self->keep_alive_ && self->server_keep_alive_ && !self->final_ec_) {
                                               self->finish_();
                                               return;
                                           }

                                           self->do_tls_shutdown_();
                                       }
            )
        );
    }

    void RestClient::do_tls_shutdown_() {
        // strand-only
        if (!in_flight_.load(std::memory_order_acquire)) return;
        if (shutting_down_) return;
        shutting_down_ = true;

        arm_shutdown_deadline_();

        auto self = shared_from_this();
        stream_->async_shutdown(
            boost::asio::bind_executor(strand_,
                                       [self](const boost::system::error_code &ec) {
                                           self->cancel_shutdown_deadline_();

                                           // Many servers do not follow TLS close_notify rules; treat EOF/truncated as non-fatal after read.
                                           if (ec &&
                                               ec != boost::asio::error::eof &&
                                               ec != boost::asio::ssl::error::stream_truncated) {
                                               self->emit_log_("[RESTCLIENT] TLS shutdown error");
                                               // Do not override primary request error if it already exists.
                                               if (!self->final_ec_) self->final_ec_ = ec;
                                           }

                                           self->finish_();
                                       }
            )
        );
    }

    void RestClient::arm_deadline_() {
        deadline_.expires_after(timeout_);
        auto self = shared_from_this();

        deadline_.async_wait(
            boost::asio::bind_executor(strand_,
                                       [self](const boost::system::error_code &ec) {
                                           if (ec) return; // canceled

                                           // Cancel resolver + socket ops; this will abort pending handlers.
                                           boost::system::error_code ignored;
                                           self->resolver_.cancel();
                                           if (self->stream_) {
                                               beast::get_lowest_layer(*self->stream_).cancel(ignored);
                                           }

                                           self->fail_(make_error_code(boost::system::errc::timed_out));
                                       }
            )
        );
    }

    void RestClient::disarm_deadline_() {
        deadline_.cancel();
    }

    void RestClient::arm_shutdown_deadline_() {
        shutdown_deadline_.expires_after(shutdown_timeout_);
        auto self = shared_from_this();

        shutdown_deadline_.async_wait(
            boost::asio::bind_executor(strand_,
                                       [self](const boost::system::error_code &ec) {
                                           if (ec) return; // canceled
                                           if (!self->in_flight_.load(std::memory_order_acquire)) return;

                                           // If shutdown hangs, ensure we return a meaningful error unless something already failed.
                                           if (!self->final_ec_) {
                                               self->final_ec_ = make_error_code(boost::system::errc::timed_out);
                                           }

                                           self->finish_();
                                       }
            )
        );
    }

    void RestClient::cancel_shutdown_deadline_() {
        shutdown_deadline_.cancel();
    }

    void RestClient::cancel() {
        auto self = shared_from_this();
        boost::asio::dispatch(strand_, [self]() {
            if (!self->in_flight_.load(std::memory_order_acquire)) {
                return; // nothing to cancel
            }

            // Prefer to report operation_aborted unless something already failed.
            if (!self->final_ec_) {
                self->final_ec_ = make_error_code(boost::system::errc::operation_canceled);
            }

            // Cancel timers + resolver + socket ops. This will cause pending handlers to abort.
            self->disarm_deadline_();
            self->cancel_shutdown_deadline_();

            boost::system::error_code ignored;
            self->resolver_.cancel();
            if (self->stream_) {
                beast::get_lowest_layer(*self->stream_).cancel(ignored);
            }

            // If TLS isn’t up yet, hard-close and finish immediately.
            if (!self->tcp_connected_ || !self->tls_handshook_) {
                self->close_socket_hard_();
                self->finish_();
                return;
            }

            // If TLS is up, attempt orderly shutdown (bounded by shutdown timeout).
            self->do_tls_shutdown_();
        });
    }
} // namespace md
