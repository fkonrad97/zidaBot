#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ssl.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/fields.hpp>     // IMPORTANT: this is where your error originates
#include <boost/beast/websocket.hpp>

#include "CmdLine.hpp"
#include "abstract/FeedHandler.hpp"
#include "md/GenericFeedHandler.hpp"
#include "utils/ProcessLoggingUtils.hpp"
#include "utils/VenueUtils.hpp"
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include "utils/DebugConfigUtils.hpp"

static void print_book_bbo(const md::OrderBook &book) {
    const Level *bb = book.bid_ptr(0);
    const Level *ba = book.ask_ptr(0);
    if (!bb || !ba) return;

    std::cout << "[BBO] bid=" << bb->priceTick << " qty=" << bb->quantityLot
            << " | ask=" << ba->priceTick << " qty=" << ba->quantityLot << "\n";
}


static std::string make_binance_ws_topic(const CmdOptions &opt,
                                         const md::FeedHandlerConfig &cfg,
                                         const std::string &ws_symbol_only_lower) {
    // Your BinanceAdapter does: "/ws/" + cfg.symbol
    // So cfg.symbol must be: "btcusdt@depth@100ms" (or similar)
    //
    // If user passes ws_path override, we don't need symbol, but we still set it for logging.

    // For now we support:
    //   --channel depth        => <sym>@depth@100ms
    //   --channel incremental  => also use depthUpdate stream; keep same
    //
    // You can later map to different channels (trade/bookTicker/etc).
    (void) cfg;

    return ws_symbol_only_lower + "@depth@100ms";
}

int main(int argc, char **argv) {
    CmdOptions options;
    if (!parse_cmdline(argc, argv, options)) {
        return 1;
    }
    if (options.show_help) {
        return 0;
    }

    std::optional<md::logging::ProcessLogSession> log_session;
    if (options.log_path && !options.log_path->empty()) {
        log_session = md::logging::enable_process_file_logging(*options.log_path);
        if (!log_session) {
            std::cerr << "Error: failed to open log file for base path " << *options.log_path << "\n";
            return 1;
        }
        std::cerr << "[MAIN] file logging enabled path=" << log_session->path << "\n";
    }

    // ---------------------------------------------------------------------
    // 1) Validate venue + stream kind
    // ---------------------------------------------------------------------
    md::VenueId venue = parse_venue(options.venue);
    if (venue == md::VenueId::UNKNOWN) {
        std::cerr << "Error: unknown venue '" << options.venue
                << "'. Expected one of: binance, okx, bybit, bitget, kucoin.\n";
        return 1;
    }

    // ---------------------------------------------------------------------
    // 2) Build config
    // ---------------------------------------------------------------------
    md::FeedHandlerConfig cfg{};
    cfg.venue_name = venue;
    cfg.base_ccy = options.base;
    cfg.quote_ccy = options.quote;

    // depthLevel: your options_description sets default_value(400),
    // so options.depthLevel should always be set in practice,
    // but keep a safe fallback.
    cfg.depthLevel = options.depthLevel.value_or(400);

    cfg.ws_host = options.ws_host.value_or("");
    cfg.ws_port = options.ws_port.value_or("");
    cfg.ws_path = options.ws_path.value_or("");
    cfg.rest_host = options.rest_host.value_or("");
    cfg.rest_port = options.rest_port.value_or("");
    cfg.rest_path = options.rest_path.value_or("");
    cfg.brain_ws_host = options.brain_ws_host.value_or("");
    cfg.brain_ws_port = options.brain_ws_port.value_or("");
    cfg.brain_ws_path = options.brain_ws_path.value_or("");
    cfg.brain_ws_insecure = options.brain_ws_insecure;
    cfg.persist_path = options.persist_path.value_or("");
    cfg.persist_book_every_updates = static_cast<std::size_t>(options.persist_book_every_updates);
    cfg.persist_book_top = static_cast<std::size_t>(options.persist_book_top);
    cfg.rest_timeout_ms = options.rest_timeout_ms;

    // Brain WS defaults (only when enabled).
    if (!cfg.brain_ws_host.empty()) {
        if (cfg.brain_ws_port.empty()) cfg.brain_ws_port = "443";
        if (cfg.brain_ws_path.empty()) cfg.brain_ws_path = "/";
    }

    /// DEBUG
    md::debug::enabled.store(options.debug, std::memory_order_relaxed);
    md::debug::raw.store(options.debug_raw, std::memory_order_relaxed);
    md::debug::every.store(options.debug_every, std::memory_order_relaxed);
    md::debug::raw_max.store(options.debug_raw_max, std::memory_order_relaxed);
    md::debug::top_levels.store(options.debug_top, std::memory_order_relaxed);
    md::debug::show_checksum.store(options.debug_checksum, std::memory_order_relaxed);
    md::debug::show_seq.store(options.debug_seq, std::memory_order_relaxed);

    // ---------------------------------------------------------------------
    // 3) Symbol mapping
    // ---------------------------------------------------------------------
    // WS symbol mapping (BINANCE => "btcusdt")
    const std::string ws_sym = md::venue::map_ws_symbol(cfg.venue_name, cfg.base_ccy, cfg.quote_ccy);
    const std::string rest_sym = md::venue::map_rest_symbol(cfg.venue_name, cfg.base_ccy, cfg.quote_ccy);

    // For your BinanceAdapter, cfg.symbol must be the full topic token
    // unless you override ws_path.
    if (cfg.ws_path.empty()) {
        if (cfg.venue_name == md::VenueId::BINANCE) {
            cfg.symbol = make_binance_ws_topic(options, cfg, ws_sym);
        } else {
            // for other venues you’ll likely use ws_sym directly or build a venue-specific topic
            cfg.symbol = ws_sym;
        }
    } else {
        // ws_path override means adapter target will be forced anyway,
        // but cfg.symbol can still be useful for logs/debug
        cfg.symbol = (cfg.venue_name == md::VenueId::BINANCE) ? make_binance_ws_topic(options, cfg, ws_sym) : ws_sym;
    }

    // ---------------------------------------------------------------------
    // 4) Debug output
    // ---------------------------------------------------------------------
    std::cerr << "[POP] Starting feed\n"
            << "  venue      = " << options.venue << "\n"
            << "  base/quote = " << cfg.base_ccy << "/" << cfg.quote_ccy << "\n"
            << "  depthLevel = " << cfg.depthLevel << "\n"
            << "  ws_sym     = " << ws_sym << "\n"
            << "  rest_sym   = " << rest_sym << "\n"
            << "  cfg.symbol = " << cfg.symbol << "\n"
            << "  ws_host    = " << (cfg.ws_host.empty() ? "<default>" : cfg.ws_host) << "\n"
            << "  ws_port    = " << (cfg.ws_port.empty() ? "<default>" : cfg.ws_port) << "\n"
            << "  ws_path    = " << (cfg.ws_path.empty() ? "<default>" : cfg.ws_path) << "\n"
            << "  rest_host  = " << (cfg.rest_host.empty() ? "<default>" : cfg.rest_host) << "\n"
            << "  rest_port  = " << (cfg.rest_port.empty() ? "<default>" : cfg.rest_port) << "\n"
            << "  rest_path  = " << (cfg.rest_path.empty() ? "<default>" : cfg.rest_path) << "\n"
            << "  brain_ws  = "
            << (cfg.brain_ws_host.empty() ? "<disabled>" : (cfg.brain_ws_host + ":" + cfg.brain_ws_port + cfg.brain_ws_path))
            << "\n"
            << "  persist    = " << (cfg.persist_path.empty() ? "<disabled>" : cfg.persist_path) << "\n"
            << "  persist_book_every_updates = " << cfg.persist_book_every_updates << "\n"
            << "  persist_book_top           = " << cfg.persist_book_top << "\n";

    // ---------------------------------------------------------------------
    // 5) Run
    // ---------------------------------------------------------------------
    boost::asio::io_context ioc;

    auto h = std::make_unique<md::GenericFeedHandler>(ioc);

    auto st = h->init(cfg);
    std::cerr << "[MAIN] init = " << (st == md::FeedOpResult::OK ? "OK" : "ERROR") << "\n";
    if (st != md::FeedOpResult::OK) return 1;

    st = h->start();
    std::cerr << "[MAIN] start = " << (st == md::FeedOpResult::OK ? "OK" : "ERROR") << "\n";
    if (st != md::FeedOpResult::OK) return 2;

    ioc.run();
    return 0;
}
