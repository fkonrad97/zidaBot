#include "connection_handler/WsClient.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ssl/error.hpp>

#include <boost/asio/ssl/rfc2818_verification.hpp>
#include <openssl/ssl.h>

namespace md {
    using tcp = boost::asio::ip::tcp;
    namespace beast = boost::beast;
    namespace websocket = beast::websocket;
    namespace ssl = boost::asio::ssl;

    WsClient::WsClient(boost::asio::io_context &ioc)
        : ioc_(ioc),
          strand_(ioc.get_executor()),
          ssl_ctx_(ssl::context::tls_client),
          ws_(ioc_, ssl_ctx_),
          resolver_(ioc_),
          connect_deadline_(ioc_),
          ping_timer_(ioc_) {
        // Disable legacy protocols; require TLS 1.2+.
        ssl_ctx_.set_options(
            ssl::context::default_workarounds |
            ssl::context::no_sslv2 |
            ssl::context::no_sslv3 |
            ssl::context::no_tlsv1 |
            ssl::context::no_tlsv1_1
        );
        // Restrict to modern AEAD cipher suites (TLS 1.2 compatible).
        // TLS 1.3 cipher suites are managed separately by OpenSSL and are
        // always strong, so no explicit TLS 1.3 filter is required.
        SSL_CTX_set_cipher_list(ssl_ctx_.native_handle(),
            "ECDHE-ECDSA-AES256-GCM-SHA384:"
            "ECDHE-RSA-AES256-GCM-SHA384:"
            "ECDHE-ECDSA-CHACHA20-POLY1305:"
            "ECDHE-RSA-CHACHA20-POLY1305:"
            "ECDHE-ECDSA-AES128-GCM-SHA256:"
            "ECDHE-RSA-AES128-GCM-SHA256");
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(ssl::verify_peer);

        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::request_type &req) {
                req.set(beast::http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " pop-wsclient");
            }
        ));
    }

    void WsClient::set_on_raw_message(RawMessageHandler h) { on_raw_message_ = std::move(h); }
    void WsClient::set_on_close(CloseHandler h) { on_close_ = std::move(h); }
    void WsClient::set_on_open(OpenHandler h) { on_open_ = std::move(h); }

    void WsClient::connect(std::string host, std::string port, std::string target) {
        auto self = shared_from_this();
        boost::asio::dispatch(strand_, [self,
                                  host = std::move(host),
                                  port = std::move(port),
                                  target = std::move(target)]() mutable {
                                  // Always make connect() a "fresh start" operation.
                                  self->stop_ping_loop_();
                                  self->disarm_connect_deadline_();

                                  boost::system::error_code ignored;
                                  self->resolver_.cancel(); // IMPORTANT: cancel any in-flight resolve
                                  self->ws_.next_layer().shutdown(ignored);
                                  beast::get_lowest_layer(self->ws_).cancel(ignored);
                                  beast::get_lowest_layer(self->ws_).close(ignored);

                                  // Reset state
                                  self->closing_ = false;
                                  self->opened_ = false;
                                  self->close_notified_.store(false, std::memory_order_release);

                                  self->outbox_.clear();
                                  self->write_in_flight_ = false;

                                  self->buffer_.consume(self->buffer_.size());
                                  self->message_.clear();

                                  self->host_ = std::move(host);
                                  self->port_ = std::move(port);
                                  self->target_ = std::move(target);

                                  self->opened_ = false;
                                  self->close_notified_.store(false, std::memory_order_release);

                                  // Configure hostname verification for this host
                                  if (self->tls_verify_peer_) {
                                      self->ws_.next_layer().set_verify_mode(ssl::verify_peer);
                                      self->ws_.next_layer().set_verify_callback(
                                          ssl::rfc2818_verification(self->host_));
                                  } else {
                                      self->ws_.next_layer().set_verify_mode(ssl::verify_none);
                                  }

                                  self->arm_connect_deadline_();
                                  self->do_resolve_();
                              });
    }

    void WsClient::do_resolve_() {
        auto self = shared_from_this();
        resolver_.async_resolve(
            host_, port_,
            boost::asio::bind_executor(strand_,
                                       [self](const beast::error_code &ec, const tcp::resolver::results_type &results) {
                                           if (self->closing_) return; // prevent late continuation
                                           if (ec) return self->fail_(ec, "resolve");
                                           self->do_tcp_connect_(results);
                                       }
            )
        );
    }

    void WsClient::do_tcp_connect_(const tcp::resolver::results_type &results) {
        auto self = shared_from_this();

        boost::asio::async_connect(
            beast::get_lowest_layer(ws_), results,
            boost::asio::bind_executor(strand_,
                                       [self](const boost::system::error_code &ec, const tcp::endpoint &) {
                                           if (self->closing_) return; // prevent late continuation
                                           if (ec) return self->fail_(ec, "tcp_connect");

                                           // SNI
                                           if (!SSL_set_tlsext_host_name(
                                               self->ws_.next_layer().native_handle(), self->host_.c_str())) {
                                               const boost::system::error_code sni_ec{
                                                   static_cast<int>(::ERR_get_error()),
                                                   boost::asio::error::get_ssl_category()
                                               };
                                               return self->fail_(sni_ec, "sni");
                                           }

                                           self->do_tls_handshake_();
                                       }
            )
        );
    }

    void WsClient::do_tls_handshake_() {
        auto self = shared_from_this();
        ws_.next_layer().async_handshake(
            ssl::stream_base::client,
            boost::asio::bind_executor(strand_,
                                       [self](const beast::error_code &ec) {
                                           if (self->closing_) return; // prevent late continuation
                                           if (ec) return self->fail_(ec, "tls_handshake");
                                           self->do_ws_handshake_();
                                       }
            )
        );
    }

    void WsClient::do_ws_handshake_() {
        auto self = shared_from_this();

        ws_.async_handshake(
            host_, target_,
            boost::asio::bind_executor(strand_,
                                       [self](const beast::error_code &ec) {
                                           if (ec) return self->fail_(ec, "ws_handshake");

                                           self->disarm_connect_deadline_();

                                           self->ws_.text(true);
                                           self->opened_ = true;

                                           self->start_write_(); // flush queued subscription frame

                                           if (self->on_open_) {
                                               try { self->on_open_(); } catch (...) {
                                               }
                                           }

                                           self->start_ping_loop_();
                                           self->do_read_();
                                       }
            )
        );
    }

    void WsClient::do_read_() {
        auto self = shared_from_this();

        ws_.async_read(
            buffer_,
            boost::asio::bind_executor(strand_,
                                       [self](const beast::error_code &ec, std::size_t /*bytes_transferred*/) {
                                           if (ec) {
                                               if (ec == websocket::error::closed) {
                                                   self->closing_ = true;
                                                   self->stop_ping_loop_();
                                                   self->disarm_connect_deadline_();
                                                   self->close_socket_hard_();
                                                   // optional but recommended for determinism
                                                   self->notify_close_once_();
                                                   return;
                                               }

                                               return self->fail_(ec, "read");
                                           }

                                           // Avoid repeated allocations by reusing message_ capacity.
                                           self->message_.assign(beast::buffers_to_string(self->buffer_.data()));

                                           if (self->on_raw_message_) {
                                               try {
                                                   self->on_raw_message_(self->message_.data(), self->message_.size());
                                               } catch (...) {
                                               }
                                           }

                                           self->buffer_.consume(self->buffer_.size());
                                           self->do_read_();
                                       }
            )
        );
    }

    void WsClient::send_text(std::string text) {
        auto self = shared_from_this();
        boost::asio::dispatch(strand_, [self, text = std::move(text)]() mutable {
            if (self->closing_) return;

            if (self->max_outbox_ == 0) return;
            while (self->outbox_.size() >= self->max_outbox_) {
                self->outbox_.pop_front();
            }
            self->outbox_.push_back(std::move(text));
            if (self->opened_) self->start_write_();
        });
    }

    void WsClient::start_write_() {
        // strand-only
        if (write_in_flight_) return;
        if (outbox_.empty()) return;
        do_write_();
    }

    void WsClient::do_write_() {
        // strand-only
        if (outbox_.empty()) return;
        write_in_flight_ = true;

        auto self = shared_from_this();
        ws_.async_write(
            boost::asio::buffer(outbox_.front()),
            boost::asio::bind_executor(strand_,
                                       [self](const beast::error_code &ec, std::size_t) {
                                           self->write_in_flight_ = false;

                                           if (ec) return self->fail_(ec, "write");

                                           self->outbox_.pop_front();
                                           self->start_write_();
                                       }
            )
        );
    }

    void WsClient::close() {
        auto self = shared_from_this();
        boost::asio::dispatch(strand_, [self] {
            if (self->closing_) return;
            self->closing_ = true;

            self->stop_ping_loop_();

            // If not opened yet, just hard-close.
            if (!self->opened_) {
                self->close_socket_hard_();
                self->notify_close_once_();
                return;
            }

            self->ws_.async_close(
                websocket::close_code::normal,
                boost::asio::bind_executor(self->strand_,
                                           [self](const beast::error_code & /*ec*/) {
                                               // We do not treat close errors as fatal at this layer; connection is ending anyway.
                                               self->close_socket_hard_();
                                               self->notify_close_once_();
                                           }
                )
            );
        });
    }

    void WsClient::cancel() {
        auto self = shared_from_this();
        boost::asio::dispatch(strand_, [self] {
            if (self->closing_) return;
            self->closing_ = true;

            self->stop_ping_loop_();

            boost::system::error_code ignored;
            self->resolver_.cancel();
            beast::get_lowest_layer(self->ws_).cancel(ignored);

            self->close_socket_hard_();
            self->notify_close_once_();
        });
    }

    void WsClient::arm_connect_deadline_() {
        connect_deadline_.expires_after(connect_timeout_);
        auto self = shared_from_this();
        connect_deadline_.async_wait(
            boost::asio::bind_executor(strand_,
                                       [self](const boost::system::error_code &ec) {
                                           if (ec) return; // canceled
                                           self->fail_(make_error_code(boost::system::errc::timed_out),
                                                       "connect_timeout");
                                       }
            )
        );
    }

    void WsClient::disarm_connect_deadline_() {
        connect_deadline_.cancel();
    }

    void WsClient::start_ping_loop_() {
        if (ping_interval_.count() <= 0) return;

        auto self = shared_from_this();
        ping_timer_.expires_after(ping_interval_);
        ping_timer_.async_wait(
            boost::asio::bind_executor(strand_,
                                       [self](const boost::system::error_code &ec) {
                                           if (ec) return; // canceled
                                           if (self->closing_ || !self->opened_) return;

                                           // websocket::stream will handle pong automatically; we just issue pings.
                                           self->ws_.async_ping(
                                               websocket::ping_data{},
                                               boost::asio::bind_executor(self->strand_,
                                                                          [self](const beast::error_code &ping_ec) {
                                                                              if (ping_ec)
                                                                                  return self->fail_(
                                                                                      ping_ec, "ping");
                                                                              self->start_ping_loop_();
                                                                          }
                                               )
                                           );
                                       }
            )
        );
    }

    void WsClient::stop_ping_loop_() {
        ping_timer_.cancel();
    }

    void WsClient::fail_(boost::system::error_code ec, std::string_view where) {
        // strand-only
        (void) where;

        if (logger_) {
            std::string msg;
            msg.reserve(64 + where.size());
            msg += "[WsClient] ";
            msg += where;
            msg += ": ";
            msg += ec.message();
            emit_log_(msg);
        }


        if (closing_) {
            notify_close_once_();
            return;
        }
        closing_ = true;

        stop_ping_loop_();
        disarm_connect_deadline_();

        // IMPORTANT: stop any in-flight resolve/connect pipeline
        resolver_.cancel(); {
            boost::system::error_code ignored;
            beast::get_lowest_layer(ws_).cancel(ignored);
        }

        // Hard-close the socket; no attempt to be graceful on error paths.
        close_socket_hard_();
        notify_close_once_();
    }

    void WsClient::notify_close_once_() {
        if (close_notified_.exchange(true, std::memory_order_acq_rel)) return;

        if (on_close_) {
            try { on_close_(); } catch (...) {
            }
        }
    }

    void WsClient::close_socket_hard_() noexcept {
        boost::system::error_code ignored;
        auto &sock = beast::get_lowest_layer(ws_);
        resolver_.cancel();
        sock.cancel(ignored);
        sock.close(ignored);
    }
} // namespace md
