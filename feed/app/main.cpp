#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ssl.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/fields.hpp>     // IMPORTANT: this is where your error originates
#include <boost/beast/websocket.hpp>

#include <iostream>
#include <spdlog/spdlog.h>

#include "CmdLine.hpp"
#include "abstract/FeedHandler.hpp"
#include "md/GenericFeedHandler.hpp"
#include "utils/HealthServer.hpp"
#include "utils/Log.hpp"
#include "utils/VenueUtils.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include "utils/DebugConfigUtils.hpp"

static void print_book_bbo(const md::OrderBook &book) {
    const Level *bb = book.bid_ptr(0);
    const Level *ba = book.ask_ptr(0);
    if (!bb || !ba) return;

    std::cout << "[BBO] bid=" << bb->priceTick << " qty=" << bb->quantityLot
            << " | ask=" << ba->priceTick << " qty=" << ba->quantityLot << "\n";
}


static std::string make_binance_ws_topic(const md::FeedHandlerConfig &cfg,
                                         const std::string &ws_symbol_only_lower) {
    (void) cfg;
    return ws_symbol_only_lower + "@depth@100ms";
}

// F3: parse "BTC/USDT,ETH/USDT,SOL/USDT" into vector of (base, quote) pairs.
static std::vector<std::pair<std::string, std::string>>
parse_symbols(const std::string &symbols_str) {
    std::vector<std::pair<std::string, std::string>> result;
    std::string token;
    std::istringstream ss(symbols_str);
    while (std::getline(ss, token, ',')) {
        // trim whitespace
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (token.empty()) continue;
        const auto slash = token.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 == token.size()) {
            throw std::invalid_argument("Invalid symbol pair (expected BASE/QUOTE): " + token);
        }
        result.emplace_back(token.substr(0, slash), token.substr(slash + 1));
    }
    return result;
}

// F3: derive per-symbol persist path from the base path.
// "/tmp/binance.jsonl" + BTC/USDT  =>  "/tmp/binance_BTC_USDT.jsonl"
// "/tmp/binance.jsonl.gz" + BTC/USDT  =>  "/tmp/binance_BTC_USDT.jsonl.gz"
// If base path is empty, returns "".
static std::string derive_persist_path(const std::string &base_path,
                                       const std::string &base_ccy,
                                       const std::string &quote_ccy) {
    if (base_path.empty()) return "";

    const std::string suffix = "_" + base_ccy + "_" + quote_ccy;

    // Find last '.' that belongs to an extension (not a directory separator).
    const auto last_sep = base_path.find_last_of("/\\");
    const auto last_dot = base_path.find_last_of('.');
    if (last_dot == std::string::npos ||
        (last_sep != std::string::npos && last_dot < last_sep)) {
        // No extension — just append suffix.
        return base_path + suffix;
    }

    // Handle double extension: ".jsonl.gz"
    const std::string ext = base_path.substr(last_dot);
    const std::string stem = base_path.substr(0, last_dot);

    // Check for ".jsonl.gz" or ".log.gz" style
    if (ext == ".gz") {
        const auto second_dot = stem.find_last_of('.');
        if (second_dot != std::string::npos &&
            (last_sep == std::string::npos || second_dot > last_sep)) {
            return stem.substr(0, second_dot) + suffix + stem.substr(second_dot) + ".gz";
        }
    }

    return stem + suffix + ext;
}

// Build a FeedHandlerConfig for one (base, quote) pair from the parsed CLI options.
static md::FeedHandlerConfig build_cfg(const CmdOptions &options,
                                       md::VenueId venue,
                                       const std::string &base,
                                       const std::string &quote,
                                       const std::string &persist_path) {
    md::FeedHandlerConfig cfg{};
    cfg.venue_name  = venue;
    cfg.base_ccy    = base;
    cfg.quote_ccy   = quote;
    cfg.depthLevel  = options.depthLevel.value_or(400);
    cfg.ws_host     = options.ws_host.value_or("");
    cfg.ws_port     = options.ws_port.value_or("");
    cfg.ws_path     = options.ws_path.value_or("");
    cfg.rest_host   = options.rest_host.value_or("");
    cfg.rest_port   = options.rest_port.value_or("");
    cfg.rest_path   = options.rest_path.value_or("");
    cfg.brain_ws_host     = options.brain_ws_host.value_or("");
    cfg.brain_ws_port     = options.brain_ws_port.value_or("");
    cfg.brain_ws_path     = options.brain_ws_path.value_or("");
    cfg.brain_ws_insecure = options.brain_ws_insecure;
    cfg.brain_ws_certfile = options.brain_ws_certfile.value_or("");
    cfg.brain_ws_keyfile  = options.brain_ws_keyfile.value_or("");
    cfg.brain2_ws_host     = options.brain2_ws_host.value_or("");
    cfg.brain2_ws_port     = options.brain2_ws_port.value_or("");
    cfg.brain2_ws_path     = options.brain2_ws_path.value_or("");
    cfg.brain2_ws_insecure = options.brain2_ws_insecure;
    cfg.brain2_ws_certfile = options.brain2_ws_certfile.value_or("");
    cfg.brain2_ws_keyfile  = options.brain2_ws_keyfile.value_or("");
    cfg.persist_path               = persist_path;
    cfg.persist_book_every_updates = static_cast<std::size_t>(options.persist_book_every_updates);
    cfg.persist_book_top           = static_cast<std::size_t>(options.persist_book_top);
    cfg.rest_timeout_ms   = options.rest_timeout_ms;
    cfg.max_msg_rate_per_sec = options.max_msg_rate_per_sec;
    cfg.validate_every    = options.validate_every;
    cfg.require_checksum  = options.require_checksum;

    // Brain WS port/path defaults.
    if (!cfg.brain_ws_host.empty()) {
        if (cfg.brain_ws_port.empty()) cfg.brain_ws_port = "443";
        if (cfg.brain_ws_path.empty()) cfg.brain_ws_path = "/";
    }
    if (!cfg.brain2_ws_host.empty()) {
        if (cfg.brain2_ws_port.empty()) cfg.brain2_ws_port = "443";
        if (cfg.brain2_ws_path.empty()) cfg.brain2_ws_path = "/";
    }

    // Symbol mapping.
    const std::string ws_sym   = md::venue::map_ws_symbol(venue, base, quote);
    const std::string rest_sym = md::venue::map_rest_symbol(venue, base, quote);

    if (cfg.ws_path.empty()) {
        cfg.symbol = (venue == md::VenueId::BINANCE)
                         ? make_binance_ws_topic(cfg, ws_sym)
                         : ws_sym;
    } else {
        cfg.symbol = (venue == md::VenueId::BINANCE)
                         ? make_binance_ws_topic(cfg, ws_sym)
                         : ws_sym;
    }

    return cfg;
}

int main(int argc, char **argv) {
    CmdOptions options;
    if (!parse_cmdline(argc, argv, options)) {
        return 1;
    }
    if (options.show_help) {
        return 0;
    }

    // D1: initialise structured logger (stderr always; file if --log_path given)
    try {
        md::log::init(options.log_level, options.log_path.value_or(""));
    } catch (const std::exception &e) {
        std::cerr << "Error: failed to open log file: " << e.what() << "\n";
        return 1;
    }

    // -------------------------------------------------------------------------
    // 1) Validate venue
    // -------------------------------------------------------------------------
    md::VenueId venue = parse_venue(options.venue);
    if (venue == md::VenueId::UNKNOWN) {
        spdlog::error("unknown venue '{}'. Expected one of: binance, okx, bybit, bitget, kucoin.",
                      options.venue);
        return 1;
    }

    // -------------------------------------------------------------------------
    // 2) Debug flags (global atomics, shared by all handlers)
    // -------------------------------------------------------------------------
    md::debug::enabled.store(options.debug, std::memory_order_relaxed);
    md::debug::raw.store(options.debug_raw, std::memory_order_relaxed);
    md::debug::every.store(options.debug_every, std::memory_order_relaxed);
    md::debug::raw_max.store(options.debug_raw_max, std::memory_order_relaxed);
    md::debug::top_levels.store(options.debug_top, std::memory_order_relaxed);
    md::debug::show_checksum.store(options.debug_checksum, std::memory_order_relaxed);
    md::debug::show_seq.store(options.debug_seq, std::memory_order_relaxed);

    // -------------------------------------------------------------------------
    // 3) Build symbol list  (F3)
    // -------------------------------------------------------------------------
    std::vector<std::pair<std::string, std::string>> symbols; // (base, quote)

    if (options.symbols.has_value()) {
        try {
            symbols = parse_symbols(*options.symbols);
        } catch (const std::exception &e) {
            spdlog::error("--symbols parse error: {}", e.what());
            return 1;
        }
    } else {
        // Fall back to single-symbol mode.
        if (options.base.empty() || options.quote.empty()) {
            spdlog::error("Either --symbols or both --base and --quote must be specified.");
            return 1;
        }
        symbols.emplace_back(options.base, options.quote);
    }

    if (symbols.empty()) {
        spdlog::error("No symbols to track. Provide --symbols or --base/--quote.");
        return 1;
    }

    const bool multi = symbols.size() > 1;
    const std::string base_persist = options.persist_path.value_or("");

    // -------------------------------------------------------------------------
    // 4) Startup log
    // -------------------------------------------------------------------------
    spdlog::info("[POP] Starting feed");
    spdlog::info("  venue      = {}", options.venue);
    spdlog::info("  symbols    = {} ({} handler{})",
                 options.symbols.value_or(options.base + "/" + options.quote),
                 symbols.size(), symbols.size() == 1 ? "" : "s");
    spdlog::info("  depthLevel = {}", options.depthLevel.value_or(400));
    spdlog::info("  brain_ws   = {}",
                 options.brain_ws_host.value_or("").empty()
                     ? "<disabled>"
                     : (options.brain_ws_host.value_or("") + ":" +
                        options.brain_ws_port.value_or("443") +
                        options.brain_ws_path.value_or("/")));
    spdlog::info("  persist    = {}", base_persist.empty()
                                          ? "<disabled>"
                                          : (multi ? base_persist + " (per-symbol derived)" : base_persist));

    // -------------------------------------------------------------------------
    // 5) Create and start one GenericFeedHandler per symbol
    //    G1: each handler gets its own io_context + std::thread so feeds are
    //    fully isolated — one slow venue cannot stall the others.
    // -------------------------------------------------------------------------
    const auto start_time = std::chrono::steady_clock::now();

    // main_ioc: used only for HealthServer + signal_set.
    boost::asio::io_context main_ioc;

    // One io_context per handler, plus a work guard so each ioc doesn't exit
    // before we explicitly tell it to stop.
    using WorkGuard = boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>;

    std::vector<std::unique_ptr<boost::asio::io_context>> handler_iocs;
    std::vector<WorkGuard>                                 work_guards;
    std::vector<std::unique_ptr<md::GenericFeedHandler>>  handlers;

    handler_iocs.reserve(symbols.size());
    work_guards.reserve(symbols.size());
    handlers.reserve(symbols.size());

    for (const auto &[base, quote] : symbols) {
        const std::string persist_path = multi
                                             ? derive_persist_path(base_persist, base, quote)
                                             : base_persist;

        md::FeedHandlerConfig cfg = build_cfg(options, venue, base, quote, persist_path);

        spdlog::info("[POP] Registering {}/{} — symbol={}, persist={}",
                     base, quote, cfg.symbol,
                     persist_path.empty() ? "<disabled>" : persist_path);

        handler_iocs.push_back(std::make_unique<boost::asio::io_context>());
        work_guards.emplace_back(
            boost::asio::make_work_guard(*handler_iocs.back()));

        auto h = std::make_unique<md::GenericFeedHandler>(*handler_iocs.back());
        auto st = h->init(cfg);
        if (st != md::FeedOpResult::OK) {
            spdlog::error("[MAIN] init failed for {}/{}", base, quote);
            return 1;
        }
        st = h->start();
        if (st != md::FeedOpResult::OK) {
            spdlog::error("[MAIN] start failed for {}/{}", base, quote);
            return 2;
        }
        handlers.push_back(std::move(h));
    }

    // Spawn one thread per handler io_context.
    std::vector<std::thread> handler_threads;
    handler_threads.reserve(handler_iocs.size());
    for (auto &ioc_ptr : handler_iocs) {
        handler_threads.emplace_back([ptr = ioc_ptr.get()] { ptr->run(); });
    }

    spdlog::info("[MAIN] {} handler{} started — each on its own thread",
                 handlers.size(), handlers.size() == 1 ? "" : "s");

    // ---- D5: Health endpoint (runs on main_ioc) ----
    // state_/ctr_resyncs_ are atomic — safe to read cross-thread.
    md::HealthServer health_server(main_ioc, options.health_port, [&]() -> std::string {
        using nlohmann::json;
        const auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();

        bool all_synced = !handlers.empty();
        json jarr = json::array();
        for (const auto &h : handlers) {
            const auto &cfg = h->config();
            const std::string state = h->sync_state_str();
            if (state != std::string("SYNCED")) all_synced = false;
            jarr.push_back({{"base", cfg.base_ccy}, {"quote", cfg.quote_ccy},
                             {"state", state}, {"running", h->is_running()},
                             {"resyncs", h->resync_count()}});
        }
        json j;
        j["ok"]       = all_synced;
        j["process"]  = "pop";
        j["venue"]    = options.venue;
        j["uptime_s"] = uptime_s;
        j["handlers"] = jarr;
        return j.dump();
    });
    health_server.start();

    // ---- Signal handling (SIGTERM / SIGINT) on main_ioc ----
    boost::asio::signal_set signals(main_ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code &, int) {
        spdlog::info("[POP] shutting down");
        health_server.stop();
        // Post stop() to each handler's own thread, then release the work
        // guard so its ioc can drain and exit cleanly.
        for (std::size_t i = 0; i < handlers.size(); ++i) {
            boost::asio::post(*handler_iocs[i],
                              [&h = *handlers[i]] { h.stop(); });
            work_guards[i].reset();
        }
        main_ioc.stop();
        md::log::flush();
    });

    main_ioc.run();

    // Wait for all handler threads to finish draining.
    for (auto &t : handler_threads) t.join();

    md::log::flush();
    return 0;
}
