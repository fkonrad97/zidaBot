#pragma once

#include "abstract/FeedHandler.hpp"

#include <boost/program_options.hpp>
#include <string>
#include <iostream>
#include <algorithm>
#include <optional>
#include <ranges>
#include <cctype>

struct CmdOptions {
    std::string venue; // required
    std::string base; // --base BTC
    std::string quote; // --quote USDT
    std::optional<int> depthLevel;
    std::optional<std::string> ws_host; // override or std::nullopt
    std::optional<std::string> ws_port; // override or std::nullopt
    std::optional<std::string> ws_path; // override or std::nullopt
    std::optional<std::string> rest_host; // override or std::nullopt
    std::optional<std::string> rest_port; // override or std::nullopt
    std::optional<std::string> rest_path; // override or std::nullopt
    std::optional<std::string> brain_ws_host; // outbound brain WS host (optional)
    std::optional<std::string> brain_ws_port; // outbound brain WS port (optional)
    std::optional<std::string> brain_ws_path; // outbound brain WS path (optional)
    bool brain_ws_insecure{false}; // disable TLS verification (local testing)
    std::optional<std::string> persist_path; // optional JSONL persistence file
    std::optional<std::string> log_path; // optional process log file path
    int persist_book_every_updates{0}; // 0 = disabled
    int persist_book_top{50}; // top N levels per side
    int rest_timeout_ms{8000}; // REST snapshot request timeout

    // Debug flags
    bool debug{false};
    bool debug_raw{false};
    int debug_every{200};
    int debug_raw_max{512};
    int debug_top{3};
    bool debug_checksum{true};
    bool debug_seq{true};

    bool show_help{false};
};

inline md::VenueId parse_venue(const std::string &v_raw) {
    std::string v = v_raw;
    std::ranges::transform(v, v.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (v == "binance") return md::VenueId::BINANCE;
    if (v == "okx") return md::VenueId::OKX;
    if (v == "bybit") return md::VenueId::BYBIT;
    if (v == "bitget") return md::VenueId::BITGET;
    if (v == "kucoin") return md::VenueId::KUCOIN;
    return md::VenueId::UNKNOWN;
}


inline bool parse_cmdline(int argc, char **argv, CmdOptions &out) {
    namespace po = boost::program_options;

    po::options_description desc("Options");
    desc.add_options()
            ("help,h", "Show this help message")
            ("venue,v", po::value<std::string>()->required(),
             "Venue name (binance, okx, bybit, bitget, kucoin)")
            ("base", po::value<std::string>()->required(),
             "Base asset, e.g. BTC")
            ("quote", po::value<std::string>()->required(),
             "Quote asset, e.g. USDT")
            ("depthLevel,dl", po::value<int>()->default_value(400),
             "Orderbook depth; required or defaults")
            ("ws_host", po::value<std::string>(),
             "Optional WebSocket host override")
            ("ws_port", po::value<std::string>(),
             "Optional WebSocket port override")
            ("ws_path", po::value<std::string>(),
             "Optional WebSocket path override")
            ("rest_host", po::value<std::string>(),
             "Optional REST host override")
            ("rest_port", po::value<std::string>(),
             "Optional REST port override")
            ("rest_path", po::value<std::string>(),
             "Optional REST path override")
            ("brain_ws_host", po::value<std::string>(),
             "Optional brain WebSocket host (PoP publishes normalized updates)")
            ("brain_ws_port", po::value<std::string>(),
             "Optional brain WebSocket port")
            ("brain_ws_path", po::value<std::string>(),
             "Optional brain WebSocket path/target, e.g. /pop")
            ("brain_ws_insecure", po::bool_switch()->default_value(false),
             "Brain WS: disable TLS cert/host verification (local testing only)")
            ("persist_path", po::value<std::string>(),
             "Optional persistence output file path (JSONL)")
            ("log_path", po::value<std::string>(),
             "Optional process log output file path (.log is appended if missing)")
            ("persist_book_every_updates", po::value<int>()->default_value(0),
             "Persist orderbook checkpoint every N applied updates (0 disables)")
            ("persist_book_top", po::value<int>()->default_value(50),
             "Orderbook checkpoint top N levels per side")
            ("rest_timeout_ms", po::value<int>()->default_value(8000),
             "REST snapshot request timeout in milliseconds")
            ("debug", po::bool_switch()->default_value(false),
             "Enable debug logging (rate-limited)")
            ("debug_raw", po::bool_switch()->default_value(false),
             "Print truncated raw WS messages on debug logs")
            ("debug_every", po::value<int>()->default_value(200),
             "Debug: print 1 message for every N parsed messages (>=1)")
            ("debug_raw_max", po::value<int>()->default_value(512),
             "Debug: max chars of raw msg to print")
            ("debug_top", po::value<int>()->default_value(3),
             "Debug: print top N levels for snapshot/update")
            ("debug_no_checksum", po::bool_switch()->default_value(false),
             "Debug: do NOT print checksum fields")
            ("debug_no_seq", po::bool_switch()->default_value(false),
             "Debug: do NOT print seq/prev fields");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << "Usage: " << argv[0]
                    << " --venue VENUE --base BTC --quote USDT "
                    << "[--depthLevel N] "
                    << "[--ws_host HOST] [--ws_port PORT] [--ws_path PATH] "
                    << "[--rest_host HOST] [--rest_port PORT] [--rest_path PATH] "
                    << "[--brain_ws_host HOST] [--brain_ws_port PORT] [--brain_ws_path PATH] [--brain_ws_insecure] "
                    << "[--persist_path FILE] [--log_path FILE] "
                    << "[--persist_book_every_updates N] [--persist_book_top N] "
                    << "[--debug --debug_raw --debug_every N --debug_top N]\n\n";
            std::cout << desc << "\n";
            out.show_help = true;
            return true;
        }

        // Enforce required options
        po::notify(vm);
    } catch (const po::error &e) {
        std::cerr << "Error parsing command line: " << e.what() << "\n\n";
        std::cerr << desc << "\n";
        return false;
    }

    out.venue = vm["venue"].as<std::string>();
    out.base = vm["base"].as<std::string>();
    out.quote = vm["quote"].as<std::string>();
    out.depthLevel = vm["depthLevel"].as<int>();
    if (vm.count("ws_host")) out.ws_host = vm["ws_host"].as<std::string>();
    if (vm.count("ws_port")) out.ws_port = vm["ws_port"].as<std::string>();
    if (vm.count("ws_path")) out.ws_path = vm["ws_path"].as<std::string>();
    if (vm.count("rest_host")) out.rest_host = vm["rest_host"].as<std::string>();
    if (vm.count("rest_port")) out.rest_port = vm["rest_port"].as<std::string>();
    if (vm.count("rest_path")) out.rest_path = vm["rest_path"].as<std::string>();
    if (vm.count("brain_ws_host")) out.brain_ws_host = vm["brain_ws_host"].as<std::string>();
    if (vm.count("brain_ws_port")) out.brain_ws_port = vm["brain_ws_port"].as<std::string>();
    if (vm.count("brain_ws_path")) out.brain_ws_path = vm["brain_ws_path"].as<std::string>();
    out.brain_ws_insecure = vm["brain_ws_insecure"].as<bool>();
    if (vm.count("persist_path")) out.persist_path = vm["persist_path"].as<std::string>();
    if (vm.count("log_path")) out.log_path = vm["log_path"].as<std::string>();
    out.persist_book_every_updates = std::max(0, vm["persist_book_every_updates"].as<int>());
    out.persist_book_top = std::max(1, vm["persist_book_top"].as<int>());
    out.rest_timeout_ms = std::max(1000, vm["rest_timeout_ms"].as<int>());

    /// DEBUG
    out.debug = vm["debug"].as<bool>();
    out.debug_raw = vm["debug_raw"].as<bool>();
    out.debug_every = std::max(1, vm["debug_every"].as<int>());
    out.debug_raw_max = std::max(0, vm["debug_raw_max"].as<int>());
    out.debug_top = std::max(0, vm["debug_top"].as<int>());

    const bool no_checksum = vm["debug_no_checksum"].as<bool>();
    const bool no_seq = vm["debug_no_seq"].as<bool>();
    out.debug_checksum = !no_checksum;
    out.debug_seq = !no_seq;

    return true;
}
