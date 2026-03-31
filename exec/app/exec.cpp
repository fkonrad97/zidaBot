<<<<<<< HEAD
// exec process entry point — to be implemented in EX6.
// Connects to brain SignalServer, runs ExecEngine with ImmediateStrategy.
// TODO(EX6): replace stub with real implementation.
int main() { return 0; }
=======
#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "brain/ArbDetector.hpp"
#include "connection_handler/WsClient.hpp"
#include "utils/Log.hpp"

#include "exec/DeadlineOrderTracker.hpp"
#include "exec/ExecCmdLine.hpp"
#include "exec/ExecEngine.hpp"
#include "exec/ImmediateStrategy.hpp"
#include "exec/StubOrderClient.hpp"

// ---------------------------------------------------------------------------
// Deserialise a JSON text frame from brain's SignalServer into an ArbCross.
// Returns false and logs on any parse error — caller should discard the frame.
// ---------------------------------------------------------------------------
static bool parse_cross(std::string_view text, brain::ArbCross &out)
{
    try {
        const auto j        = nlohmann::json::parse(text);
        out.sell_venue       = j.at("sell_venue").get<std::string>();
        out.buy_venue        = j.at("buy_venue").get<std::string>();
        out.sell_bid_tick    = j.at("sell_bid_tick").get<std::int64_t>();
        out.buy_ask_tick     = j.at("buy_ask_tick").get<std::int64_t>();
        out.spread_bps       = j.at("spread_bps").get<double>();
        out.ts_detected_ns   = j.at("ts_detected_ns").get<std::int64_t>();
        out.sell_ts_book_ns  = j.at("sell_ts_book_ns").get<std::int64_t>();
        out.buy_ts_book_ns   = j.at("buy_ts_book_ns").get<std::int64_t>();
        return true;
    } catch (const nlohmann::json::exception &e) {
        spdlog::warn("[exec] parse_cross error: {}", e.what());
        return false;
    }
}

int main(int argc, char **argv)
{
    exec::ExecOptions opts;
    if (!exec::parse_exec_cmdline(argc, argv, opts))
        return 1;
    if (opts.show_help)
        return 0;

    md::log::init(opts.log_level);

    // ---- I/O context + exec strand ----
    // All exec state (engine, tracker, strategy) lives on this strand.
    // WsClient has its own internal strand; on_raw_message is dispatched
    // onto the exec strand before touching any exec state.
    boost::asio::io_context ioc;
    auto strand = boost::asio::make_strand(ioc);

    // ---- Component assembly ----
    exec::StubOrderClient client(strand);

    // EX5: per-order deadline timer. on_timeout fires on the exec strand.
    // ExecEngine is not yet reachable from the lambda (constructed below),
    // so we log only for now. EX7 will wire pause() through a shared_ptr.
    auto tracker = exec::DeadlineOrderTracker::create(
        strand,
        std::chrono::milliseconds(opts.confirmation_timeout_ms),
        [&opts](const std::string &oid) {
            spdlog::warn("[exec] order timeout oid={} venue={} — no fill within {}ms",
                         oid, opts.venue, opts.confirmation_timeout_ms);
        });

    // EX3: ImmediateStrategy — submits one order per ArbCross leg.
    auto strategy = std::make_unique<exec::ImmediateStrategy>(
        opts.venue,
        client,
        *tracker,
        strand,
        opts.target_qty);

    // EX4: ExecEngine — enforces E1–E4 guards then dispatches to strategy.
    exec::ExecEngine engine(
        opts.venue,
        std::move(strategy),
        opts.position_limit,
        opts.max_order_notional,
        opts.cooldown_ms * 1'000'000LL); // ms → ns

    // ---- WsClient: inbound signal stream from brain's SignalServer ----
    auto ws = md::WsClient::create(ioc);

    if (opts.insecure_tls)
        ws->set_tls_verify_peer(false);
    if (!opts.certfile.empty())
        ws->set_client_cert(opts.certfile, opts.keyfile);

    // WsClient fires on_raw_message on its own internal strand.
    // Dispatch onto exec strand before touching engine so all exec state
    // is always accessed from the same serialised executor.
    ws->set_on_raw_message(
        [&engine, &strand](const char *data, std::size_t size) {
            std::string text(data, size);
            boost::asio::dispatch(strand,
                [&engine, text = std::move(text)]() mutable {
                    brain::ArbCross cross;
                    if (parse_cross(text, cross))
                        engine.on_signal(cross);
                });
        });

    ws->set_on_open([&opts]() {
        spdlog::info("[exec] connected to brain signal server "
                     "venue={} host={}:{}",
                     opts.venue, opts.brain_host, opts.brain_port);
    });

    ws->set_on_close([&opts]() {
        spdlog::warn("[exec] disconnected from brain signal server venue={}",
                     opts.venue);
    });

    ws->connect(opts.brain_host, std::to_string(opts.brain_port), "/");

    // ---- Graceful shutdown on SIGINT / SIGTERM ----
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code &, int) {
            spdlog::info("[exec] shutting down venue={}", opts.venue);
            // Cancel all pending order deadline timers before stopping ioc
            // so their async_wait handlers complete cleanly with operation_aborted.
            boost::asio::dispatch(strand, [&tracker, &ws, &ioc]() {
                tracker->cancel_all();
                ws->close();
                ioc.stop();
            });
            md::log::flush();
        });

    spdlog::info("[exec] starting venue={} brain={}:{} "
                 "target_qty={} position_limit={} max_order_notional={} "
                 "cooldown_ms={} confirmation_timeout_ms={}",
                 opts.venue, opts.brain_host, opts.brain_port,
                 opts.target_qty, opts.position_limit,
                 opts.max_order_notional, opts.cooldown_ms,
                 opts.confirmation_timeout_ms);

    ioc.run();
    return 0;
}
>>>>>>> 37e2fba (Refactor exec process and implement order tracking)
