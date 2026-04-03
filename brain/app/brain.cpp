#include <csignal>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
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
#include "brain/SignalServer.hpp"

// ---------------------------------------------------------------------------
// G4: Health snapshot — written by the scan thread after every event,
// read by the health/watchdog callbacks on the I/O thread.
// ---------------------------------------------------------------------------
struct VenueSnap
{
    std::string venue_name;
    std::string symbol;
    std::string state; // "synced" | "feed_down" | "syncing"
    bool feed_healthy{false};
    std::int64_t ts_book_ns{0};
};

struct HealthSnapshot
{
    std::size_t synced{0};
    std::size_t total{0};
    std::int64_t last_cross_ns{0};
    brain::LatencyHistogram::Percentiles latency{};
    bool standby{false};
    std::vector<VenueSnap> venues;
};

static std::string serialize_cross(const brain::ArbCross &c)
{
    nlohmann::json j;
    j["schema_version"]  = 1;           // MED-7
    j["event_type"]      = "arb_cross"; // MED-7
    j["ts_detected_ns"]  = c.ts_detected_ns;
    j["sell_venue"]      = c.sell_venue;
    j["buy_venue"]       = c.buy_venue;
    j["sell_bid_tick"]   = c.sell_bid_tick;
    j["buy_ask_tick"]    = c.buy_ask_tick;
    j["spread_bps"]      = c.spread_bps;
    j["net_spread_bps"]  = c.net_spread_bps;
    j["sell_ts_book_ns"] = c.sell_ts_book_ns;
    j["buy_ts_book_ns"]  = c.buy_ts_book_ns;
    return j.dump();
}

int main(int argc, char **argv)
{
    brain::BrainOptions opts;
    if (!brain::parse_brain_cmdline(argc, argv, opts))
        return 1;
    if (opts.show_help)
        return 0;

    // D1: initialise structured logger before any other output
    md::log::init(opts.log_level);

    // ---- TLS context ----
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_server);
    try
    {
        ssl_ctx.use_certificate_chain_file(opts.certfile);
        ssl_ctx.use_private_key_file(opts.keyfile, boost::asio::ssl::context::pem);
        ssl_ctx.set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::no_tlsv1 |
            boost::asio::ssl::context::no_tlsv1_1);
        SSL_CTX_set_cipher_list(ssl_ctx.native_handle(),
                                "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
                                "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
                                "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256");

        // F1: mTLS — if a CA cert is provided, require PoP clients to present a
        // certificate signed by that CA.  Without --ca-certfile, brain still accepts
        // any TLS connection (backward-compatible default).
        if (!opts.ca_certfile.empty())
        {
            ssl_ctx.load_verify_file(opts.ca_certfile);
            ssl_ctx.set_verify_mode(
                boost::asio::ssl::verify_peer |
                boost::asio::ssl::verify_fail_if_no_peer_cert);
            spdlog::info("[brain] mTLS enabled: client cert required, CA={}", opts.ca_certfile);
        }
    }
    catch (const std::exception &e)
    {
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
        opts.max_age_ms * 1'000'000LL,
        opts.max_price_deviation_pct,
        opts.output,
        output_max_bytes,
        opts.venue_fee_bps);
    // F4: standby mode — suppress signal emission until promoted via SIGUSR1
    if (opts.standby)
    {
        arb.set_active(false);
        spdlog::warn("[brain] starting in STANDBY mode — promote with: kill -USR1 {}", ::getpid());
    }

    // ---- G4: Event queue shared between I/O thread and scan thread ----
    // I/O thread deserializes and enqueues; scan thread owns book + arb.
    constexpr std::size_t kEventQueueCap = 50'000;
    std::mutex event_mu;
    std::condition_variable event_cv;
    std::deque<nlohmann::json> event_queue;
    std::atomic<bool> scan_running{true};

    // ---- G4: Health snapshot shared between scan thread and I/O callbacks ----
    std::mutex health_mu;
    HealthSnapshot health_snapshot;

    // Helper: scan thread calls this after every processed event to keep the
    // health/watchdog callbacks current without touching book/arb directly.
    auto update_health_snapshot = [&]()
    {
        const auto &vs = book.venues();
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();

        HealthSnapshot snap;
        snap.total = vs.size();
        snap.synced = book.synced_count();
        snap.last_cross_ns = arb.last_cross_ns();
        snap.latency = arb.latency_percentiles();
        snap.standby = !arb.is_active();
        snap.venues.reserve(vs.size());
        for (const auto &vb : vs)
        {
            std::string st;
            if (vb.synced())
                st = "synced";
            else if (!vb.feed_healthy)
                st = "feed_down";
            else
                st = "syncing";
            snap.venues.push_back({vb.venue_name, vb.symbol, st,
                                   vb.feed_healthy, vb.ts_book_ns});
            (void)now_ns; // ts_book_ns stored directly; age_ms computed at read time
        }
        std::lock_guard lk(health_mu);
        health_snapshot = std::move(snap);
    };

    // ---- G4: Scan thread ----
    std::thread scan_thread([&]()
                            {
        spdlog::info("[brain] scan thread started");
        while (scan_running.load(std::memory_order_relaxed)) {
            nlohmann::json j;
            {
                std::unique_lock lk(event_mu);
                event_cv.wait(lk, [&] {
                    return !event_queue.empty() || !scan_running.load(std::memory_order_relaxed);
                });
                if (event_queue.empty()) break; // scan_running went false
                j = std::move(event_queue.front());
                event_queue.pop_front();
            }
            try {
                const std::string updated = book.on_event(j);
                if (!updated.empty() && book.synced_count() >= 2)
                    arb.scan(book.venues());
            } catch (...) {}
            update_health_snapshot();
        }
        spdlog::info("[brain] scan thread exiting"); });

    // ---- Message callback (I/O thread) ----
    // H1: PoP sends MessagePack binary frames; fall back to JSON text for
    // backward compatibility during mixed-version deployments.
    // G4: only deserialize here — push to queue for scan thread.
    auto on_message = [&](std::string_view msg, bool is_binary)
    {
        try
        {
            nlohmann::json j;
            if (is_binary)
            {
                j = nlohmann::json::from_msgpack(
                    reinterpret_cast<const uint8_t *>(msg.data()),
                    reinterpret_cast<const uint8_t *>(msg.data()) + msg.size());
            }
            else
            {
                j = nlohmann::json::parse(msg);
            }
            {
                std::lock_guard lk(event_mu);
                while (event_queue.size() >= kEventQueueCap)
                    event_queue.pop_front(); // drop oldest — prefer freshness
                event_queue.push_back(std::move(j));
            }
            event_cv.notify_one();
        }
        catch (const nlohmann::json::exception &e)
        {
            spdlog::warn("[brain] parse error: {}", e.what());
        }
        catch (...)
        {
        }
    };

    // ---- Server ----
    const auto addr = boost::asio::ip::make_address(opts.bind);
    const boost::asio::ip::tcp::endpoint ep(addr, opts.port);

    brain::WsServer server(ioc, ssl_ctx, ep, on_message);
    server.start();

    // ---- ES2: optional outbound signal channel — pushes ArbCross JSON to exec processes.
    // MED-4: on_cross_ is assigned here, before ioc.run() and before any events are
    // processed by the scan thread, ensuring the callback is visible on first scan.
    // Disabled when --signal-port is 0 (default).
    std::unique_ptr<brain::SignalServer> signal_server;
    if (opts.signal_port > 0)
    {
        const boost::asio::ip::tcp::endpoint signal_ep(addr, opts.signal_port);
        signal_server = std::make_unique<brain::SignalServer>(ioc, ssl_ctx, signal_ep);
        signal_server->start();
        spdlog::info("[brain] signal server listening on port {}", opts.signal_port);

        arb.on_cross_ = [&signal_server](const brain::ArbCross &c)
        {
            signal_server->broadcast(serialize_cross(c));
        };
    }

    // ---- D4: Brain watchdog (I/O thread — reads health snapshot) ----
    boost::asio::steady_timer watchdog_timer(ioc);
    const std::int64_t watchdog_no_cross_ns =
        opts.watchdog_no_cross_sec > 0
            ? opts.watchdog_no_cross_sec * 1'000'000'000LL
            : 0;

    std::function<void()> arm_watchdog;
    arm_watchdog = [&]()
    {
        watchdog_timer.expires_after(std::chrono::seconds(60));
        watchdog_timer.async_wait([&](const boost::system::error_code &ec)
                                  {
            if (ec) return;

            // G4: read from snapshot — scan thread owns book/arb directly.
            HealthSnapshot snap;
            { std::lock_guard lk(health_mu); snap = health_snapshot; }

            if (snap.synced == 0) {
                spdlog::warn("[brain] WATCHDOG: no synced venues (synced_count=0)");
            } else if (snap.synced == 1) {
                spdlog::info("[brain] WATCHDOG: only 1 synced venue, arb scan suspended");
            }

            if (watchdog_no_cross_ns > 0 && snap.synced >= 2 && snap.last_cross_ns > 0) {
                const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                if (now - snap.last_cross_ns > watchdog_no_cross_ns) {
                    spdlog::warn("[brain] WATCHDOG: no arb cross in {}s (threshold={}s)",
                                 (now - snap.last_cross_ns) / 1'000'000'000LL,
                                 opts.watchdog_no_cross_sec);
                }
            }

            arm_watchdog(); });
    };
    arm_watchdog();

    // ---- D5: Health endpoint (I/O thread — reads health snapshot) ----
    md::HealthServer health_server(ioc, opts.health_port, [&]() -> std::string
                                   {
        using nlohmann::json;
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();

        // G4: read from snapshot — thread-safe copy under health_mu.
        HealthSnapshot snap;
        { std::lock_guard lk(health_mu); snap = health_snapshot; }

        json j;
        j["ok"]       = (snap.total > 0 && snap.synced == snap.total);
        j["process"]  = "brain";
        j["uptime_s"] = uptime_s;
        j["synced"]   = snap.synced;
        j["total"]    = snap.total;

        json jarr = json::array();
        for (const auto &vb : snap.venues) {
            const std::int64_t age_ms = vb.ts_book_ns > 0
                ? (now_ns - vb.ts_book_ns) / 1'000'000LL : 0;
            jarr.push_back({{"venue", vb.venue_name}, {"symbol", vb.symbol},
                             {"state", vb.state}, {"feed_healthy", vb.feed_healthy},
                             {"age_ms", age_ms}});
        }
        j["venues"] = jarr;

        if (snap.last_cross_ns > 0)
            j["last_cross_s_ago"] = (now_ns - snap.last_cross_ns) / 1'000'000'000.0;
        else
            j["last_cross_s_ago"] = nullptr;

        j["ws_clients"] = server.session_count();
        j["standby"]    = snap.standby; // F4

        // F5: detection latency percentiles (null when no crosses yet)
        if (snap.latency.n == 0) {
            j["latency_us"] = nullptr;
        } else {
            j["latency_us"] = {{"p50", snap.latency.p50_us}, {"p95", snap.latency.p95_us},
                               {"p99", snap.latency.p99_us}, {"n",   snap.latency.n}};
        }
        return j.dump(); });
    health_server.start();

    // ---- Signal handling ----
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code &, int)
                       {
        spdlog::info("[brain] shutting down");
        watchdog_timer.cancel();
        // G4: stop scan thread before flushing arb
        scan_running.store(false, std::memory_order_relaxed);
        event_cv.notify_all();
        health_server.stop();
        server.stop();
        if (signal_server) signal_server->stop();
        ioc.stop();
        md::log::flush(); });

    // F4: SIGUSR1 promotes standby brain to active
    boost::asio::signal_set promote_signal(ioc, SIGUSR1);
    promote_signal.async_wait([&](const boost::system::error_code &ec, int)
                              {
        if (ec) return;
        arb.set_active(true);
        spdlog::warn("[brain] PROMOTED to active (SIGUSR1 received)"); });

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

    if (!opts.venue_fee_bps.empty()) {
        for (const auto &[venue, fee] : opts.venue_fee_bps)
            spdlog::info("[brain] venue fee: {}={}bps", venue, fee);
    } else {
        spdlog::info("[brain] venue fees: none configured (net_spread == raw spread)");
    }

    // G2: run I/O thread pool — safe because all WsSession handlers use strands.
    const unsigned n_ioc_threads =
        std::max(2u, std::min(4u, std::thread::hardware_concurrency()));
    spdlog::info("[brain] I/O thread pool: {} threads", n_ioc_threads);

    std::vector<std::thread> ioc_threads;
    ioc_threads.reserve(n_ioc_threads);
    for (unsigned i = 0; i < n_ioc_threads; ++i)
        ioc_threads.emplace_back([&ioc]
                                 { ioc.run(); });
    for (auto &t : ioc_threads)
        t.join();

    // G4: wait for scan thread to drain and exit.
    if (scan_thread.joinable())
        scan_thread.join();
    // arb.flush() is not called here — ArbDetector destructor calls flush()
    // via ~ArbDetector() → flush(). Calling it twice would double-log stats
    // and risks closing the output file before the writer thread fully drains.
    md::log::flush();
    return 0;
}
