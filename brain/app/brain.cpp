#include <csignal>
#include <iostream>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/context.hpp>

#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

#include "brain/ArbDetector.hpp"
#include "brain/BrainCmdLine.hpp"
#include "brain/UnifiedBook.hpp"
#include "brain/WsServer.hpp"

int main(int argc, char **argv) {
    brain::BrainOptions opts;
    if (!brain::parse_brain_cmdline(argc, argv, opts)) return 1;
    if (opts.show_help) return 0;

    // ---- TLS context ----
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_server);
    try {
        ssl_ctx.use_certificate_chain_file(opts.certfile);
        ssl_ctx.use_private_key_file(opts.keyfile, boost::asio::ssl::context::pem);
        ssl_ctx.set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::no_tlsv1 |
            boost::asio::ssl::context::no_tlsv1_1
        );
        SSL_CTX_set_cipher_list(ssl_ctx.native_handle(),
            "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
            "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
            "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256");
    } catch (const std::exception &e) {
        std::cerr << "[brain] TLS setup error: " << e.what() << "\n";
        return 1;
    }
    // Brain does not verify client certificates (PoP doesn't present one)

    // ---- Core components ----
    boost::asio::io_context ioc;

    brain::UnifiedBook book(opts.depth);
    brain::ArbDetector arb(
        opts.min_spread_bps,
        opts.max_age_ms * 1'000'000LL,
        opts.output
    );

    // ---- Message callback ----
    auto on_message = [&](std::string_view msg) {
        try {
            const auto j = nlohmann::json::parse(msg);
            const std::string updated = book.on_event(j);
            if (!updated.empty() && book.synced_count() >= 2)
                arb.scan(book.venues());
        } catch (const nlohmann::json::exception &e) {
            std::cerr << "[brain] JSON parse error: " << e.what() << "\n";
        } catch (...) {}
    };

    // ---- Server ----
    const auto addr = boost::asio::ip::make_address(opts.bind);
    const boost::asio::ip::tcp::endpoint ep(addr, opts.port);

    brain::WsServer server(ioc, ssl_ctx, ep, on_message);
    server.start();

    // ---- Signal handling ----
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code &, int) {
        std::cerr << "[brain] shutting down\n";
        arb.flush();
        server.stop();
        ioc.stop();
    });

    std::cerr << "[brain] running (depth=" << opts.depth
              << " min_spread=" << opts.min_spread_bps << "bps"
              << " max_age=" << opts.max_age_ms << "ms)\n";

    ioc.run();
    return 0;
}
