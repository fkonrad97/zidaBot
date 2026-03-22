#include <csignal>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>

#include "brain/ArbDetector.hpp"
#include "brain/BrainCmdLine.hpp"
#include "brain/UnifiedBook.hpp"
#include "brain/WsServer.hpp"
#include "utils/HealthServer.hpp"
#include "utils/Log.hpp"

int main(int argc, char **argv) {
    brain::BrainOptions opts;
    if (!brain::parse_brain_cmdline(argc, argv, opts)) return 1;
    if (opts.show_help) return 0;

    // D1: initialise structured logger before any other output
    md::log::init(opts.log_level);

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

        // F1: mTLS — if a CA cert is provided, require PoP clients to present a
        // certificate signed by that CA.  Without --ca-certfile, brain still accepts
        // any TLS connection (backward-compatible default).
        if (!opts.ca_certfile.empty()) {
            ssl_ctx.load_verify_file(opts.ca_certfile);
            ssl_ctx.set_verify_mode(
                boost::asio::ssl::verify_peer |
                boost::asio::ssl::verify_fail_if_no_peer_cert
            );
            spdlog::info("[brain] mTLS enabled: client cert required, CA={}", opts.ca_certfile);
        }
    } catch (const std::exception &e) {
        spdlog::error("[brain] TLS setup error: {}", e.what());
        return 1;
    }

    // ---- Core components ----
    boost::asio::io_context ioc;
    const auto start_time = std::chrono::steady_clock::now();

    brain::UnifiedBook book(opts.depth);
    const std::uint64_t output_max_bytes =
        opts.output_max_mb > 0 ? opts.output_max_mb * 1024ULL * 1024ULL : 0;

    brain::ArbDetector arb(
        opts.min_spread_bps,
        opts.max_spread_bps,
        opts.rate_limit_ms * 1'000'000LL,
        opts.max_age_ms    * 1'000'000LL,
        opts.max_price_deviation_pct,
        opts.output,
        output_max_bytes
    );
    // F4: standby mode — suppress signal emission until promoted via SIGUSR1
    if (opts.standby) {
        arb.set_active(false);
        spdlog::warn("[brain] starting in STANDBY mode — promote with: kill -USR1 {}", ::getpid());
    }

    // ---- Message callback ----
    // H1: PoP sends MessagePack binary frames; fall back to JSON text for
    // backward compatibility during mixed-version deployments.
    auto on_message = [&](std::string_view msg, bool is_binary) {
        try {
            nlohmann::json j;
            if (is_binary) {
                j = nlohmann::json::from_msgpack(
                        reinterpret_cast<const uint8_t *>(msg.data()),
                        reinterpret_cast<const uint8_t *>(msg.data()) + msg.size());
            } else {
                j = nlohmann::json::parse(msg);
            }
            const std::string updated = book.on_event(j);
            if (!updated.empty() && book.synced_count() >= 2)
                arb.scan(book.venues());
        } catch (const nlohmann::json::exception &e) {
            spdlog::warn("[brain] parse error: {}", e.what());
        } catch (...) {}
    };

    // ---- Server ----
    const auto addr = boost::asio::ip::make_address(opts.bind);
    const boost::asio::ip::tcp::endpoint ep(addr, opts.port);

    brain::WsServer server(ioc, ssl_ctx, ep, on_message);
    server.start();

    // ---- D4: brain watchdog ----
    boost::asio::steady_timer watchdog_timer(ioc);
    const std::int64_t watchdog_no_cross_ns =
        opts.watchdog_no_cross_sec > 0
            ? opts.watchdog_no_cross_sec * 1'000'000'000LL
            : 0;

    std::function<void()> arm_watchdog;
    arm_watchdog = [&]() {
        watchdog_timer.expires_after(std::chrono::seconds(60));
        watchdog_timer.async_wait([&](const boost::system::error_code &ec) {
            if (ec) return;

            const std::size_t synced = book.synced_count();
            if (synced == 0) {
                spdlog::warn("[brain] WATCHDOG: no synced venues (synced_count=0)");
            } else if (synced == 1) {
                spdlog::info("[brain] WATCHDOG: only 1 synced venue, arb scan suspended");
            }

            if (watchdog_no_cross_ns > 0 && synced >= 2) {
                const std::int64_t last = arb.last_cross_ns();
                if (last > 0) {
                    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    if (now - last > watchdog_no_cross_ns) {
                        spdlog::warn("[brain] WATCHDOG: no arb cross in {}s (threshold={}s)",
                                     (now - last) / 1'000'000'000LL, opts.watchdog_no_cross_sec);
                    }
                }
            }

            arm_watchdog();
        });
    };
    arm_watchdog();

    // ---- D5: Health endpoint ----
    md::HealthServer health_server(ioc, opts.health_port, [&]() -> std::string {
        using nlohmann::json;
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();

        const auto &venues = book.venues();
        const std::size_t total = venues.size();
        const std::size_t synced = book.synced_count();

        json j;
        j["ok"]       = (total > 0 && synced == total);
        j["process"]  = "brain";
        j["uptime_s"] = uptime_s;
        j["synced"]   = synced;
        j["total"]    = total;

        json jarr = json::array();
        for (const auto &vb : venues) {
            std::string state;
            if (vb.synced())           state = "synced";
            else if (!vb.feed_healthy) state = "feed_down";
            else                       state = "syncing";
            const std::int64_t age_ms = vb.ts_book_ns > 0
                ? (now_ns - vb.ts_book_ns) / 1'000'000LL : 0;
            jarr.push_back({{"venue", vb.venue_name}, {"symbol", vb.symbol},
                             {"state", state}, {"feed_healthy", vb.feed_healthy},
                             {"age_ms", age_ms}});
        }
        j["venues"] = jarr;

        const std::int64_t last_cross = arb.last_cross_ns();
        if (last_cross > 0)
            j["last_cross_s_ago"] = (now_ns - last_cross) / 1'000'000'000.0;
        else
            j["last_cross_s_ago"] = nullptr;

        j["ws_clients"] = server.session_count();
        j["standby"]    = !arb.is_active(); // F4: true when in passive standby mode

        // F5: detection latency percentiles (null when no crosses yet)
        if (!arb.latency_percentiles().n) {
            j["latency_us"] = nullptr;
        } else {
            const auto lp = arb.latency_percentiles();
            j["latency_us"] = {{"p50", lp.p50_us}, {"p95", lp.p95_us},
                               {"p99", lp.p99_us}, {"n",   lp.n}};
        }
        return j.dump();
    });
    health_server.start();

    // ---- Signal handling ----
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code &, int) {
        spdlog::info("[brain] shutting down");
        watchdog_timer.cancel();
        arb.flush();
        health_server.stop();
        server.stop();
        ioc.stop();
        md::log::flush();
    });

    // F4: SIGUSR1 promotes standby brain to active
    boost::asio::signal_set promote_signal(ioc, SIGUSR1);
    promote_signal.async_wait([&](const boost::system::error_code &ec, int) {
        if (ec) return;
        arb.set_active(true);
        spdlog::warn("[brain] PROMOTED to active (SIGUSR1 received)");
    });

    spdlog::info("[brain] running depth={} min_spread={}bps max_spread={} rate_limit={} "
                 "max_age={}ms max_price_dev={} output_max={} watchdog_no_cross={} mtls={}",
        opts.depth,
        opts.min_spread_bps,
        opts.max_spread_bps > 0.0 ? std::to_string(opts.max_spread_bps) + "bps" : "no-cap",
        opts.rate_limit_ms > 0 ? std::to_string(opts.rate_limit_ms) + "ms" : "off",
        opts.max_age_ms,
        opts.max_price_deviation_pct > 0.0 ? std::to_string(opts.max_price_deviation_pct) + "%" : "off",
        opts.output_max_mb > 0 ? std::to_string(opts.output_max_mb) + "MB" : "no-rotation",
        opts.watchdog_no_cross_sec > 0 ? std::to_string(opts.watchdog_no_cross_sec) + "s" : "off",
        opts.ca_certfile.empty() ? "off" : "on");

    ioc.run();
    return 0;
}
