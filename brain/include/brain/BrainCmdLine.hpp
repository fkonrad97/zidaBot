#pragma once

#include <algorithm>
#include <boost/program_options.hpp>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace brain {

inline std::string trim_ascii_whitespace(std::string s) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    const auto begin = std::find_if(s.begin(), s.end(), not_space);
    if (begin == s.end()) return {};
    const auto end = std::find_if(s.rbegin(), s.rend(), not_space).base();
    return std::string(begin, end);
}

/// Parse a comma-separated "venue:fee_bps" string into a map.
/// Example: "binance:10,okx:8,bybit:10"  →  {{"binance",10},{"okx",8},{"bybit",10}}
/// Malformed or unsafe tokens are skipped with a warning.
inline std::unordered_map<std::string, double> parse_venue_fees(const std::string &s) {
    std::unordered_map<std::string, double> result;
    if (s.empty()) return result;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        const auto colon = token.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == token.size()) {
            spdlog::warn("[brain] venue-fees: malformed token '{}' skipped", token);
            continue;
        }
        const std::string venue = trim_ascii_whitespace(token.substr(0, colon));
        const std::string fee_text = trim_ascii_whitespace(token.substr(colon + 1));
        if (venue.empty() || fee_text.empty()) {
            spdlog::warn("[brain] venue-fees: malformed token '{}' skipped", token);
            continue;
        }
        try {
            const double fee = std::stod(fee_text);
            if (fee < 0.0) {
                spdlog::warn("[brain] venue-fees: negative fee ignored for '{}'", venue);
                continue;
            }
            if (fee > 1000.0) {
                spdlog::warn("[brain] venue-fees: suspiciously large fee {:.1f}bps for '{}' — accepted but verify",
                             fee, venue);
            }
            result[venue] = fee;
        } catch (...) {
            spdlog::warn("[brain] venue-fees: malformed token '{}' skipped", token);
        }
    }
    return result;
}

struct BrainOptions {
    std::string bind{"0.0.0.0"};
    uint16_t    port{8443};
    std::string certfile;       ///< Required: TLS cert PEM to present to PoP clients
    std::string keyfile;        ///< Required: TLS private key PEM
    std::string ca_certfile;    ///< F1: optional CA cert for mTLS client verification
    std::string output;         ///< Optional: arb signal JSONL output file path
    std::string log_level{"info"};  ///< D1: log verbosity: debug | info | warn | error
    double      min_spread_bps{0.0};
    double      max_spread_bps{0.0};  ///< 0 = no upper cap; >0 logs anomaly + suppresses
    std::int64_t rate_limit_ms{1000}; ///< min ms between signals for same (sell,buy) pair; 0 = off
    std::int64_t max_age_ms{5000};
    double      max_price_deviation_pct{0.0}; ///< B5: 0 = disabled; skip venues deviating > N % from median
    std::size_t output_max_mb{0};             ///< D3: rotate arb output after N MB (0 = no rotation)
    std::int64_t watchdog_no_cross_sec{0};    ///< D4: warn if no cross in this many seconds (0 = disabled)
    std::size_t depth{50};      ///< OrderBook depth per venue
    uint16_t    health_port{0};   ///< D5: plain-HTTP health endpoint port (0 = disabled)
    bool        standby{false};   ///< F4: start in passive standby mode (no signal emission)
    uint16_t signal_port{0};  ///< ES2: outbound exec signal port (0 = disabled)
    /// Per-venue taker fee in bps, parsed from --venue-fees.
    /// Used by ArbDetector to compute fee-adjusted net spread.
    std::unordered_map<std::string, double> venue_fee_bps;
    bool        show_help{false};
};

inline bool parse_brain_cmdline(int argc, char **argv, BrainOptions &out) {
    namespace po = boost::program_options;

    po::options_description desc("brain");
    desc.add_options()
        ("help,h", "Show this help message")
        ("config",        po::value<std::string>()->default_value(""),
                          "F2: config file path (key=value per line; CLI flags override file values)")
        ("bind",          po::value<std::string>()->default_value("0.0.0.0"),
                          "Bind address")
        ("port",          po::value<uint16_t>()->default_value(8443),
                          "Listen port (WSS)")
        ("certfile",      po::value<std::string>(),
                          "TLS certificate PEM file")
        ("keyfile",       po::value<std::string>(),
                          "TLS private key PEM file")
        ("ca-certfile",   po::value<std::string>(),
                          "F1: CA certificate PEM for mTLS — require PoP clients to present a cert signed by this CA")
        ("output",        po::value<std::string>(),
                          "Arb signal output JSONL file path")
        ("min-spread-bps",po::value<double>()->default_value(0.0),
                          "Minimum spread in bps to emit (default 0 = all crosses)")
        ("max-spread-bps",po::value<double>()->default_value(0.0),
                          "Upper spread cap in bps; crosses above this are logged as anomalies and suppressed (0 = no cap)")
        ("rate-limit-ms", po::value<std::int64_t>()->default_value(1000),
                          "Min milliseconds between arb signals for the same venue pair (0 = unlimited)")
        ("max-age-ms",    po::value<std::int64_t>()->default_value(5000),
                          "Max individual book age and max book-age diff ms for cross-venue staleness guard")
        ("max-price-deviation-pct", po::value<double>()->default_value(0.0),
                          "B5: skip venues whose best_bid deviates more than N% from median (0=disabled)")
        ("output-max-mb", po::value<std::size_t>()->default_value(0),
                          "D3: rotate arb output file after N MB (0=no rotation)")
        ("watchdog-no-cross-sec", po::value<std::int64_t>()->default_value(0),
                          "D4: log WARN if no arb cross detected in this many seconds (0=disabled)")
        ("depth",         po::value<std::size_t>()->default_value(50),
                          "OrderBook depth per venue")
        ("log-level",     po::value<std::string>()->default_value("info"),
                          "D1: log verbosity: debug | info | warn | error")
        ("health-port",   po::value<uint16_t>()->default_value(0),
                          "D5: plain-HTTP health endpoint port (0 = disabled); e.g. 8081")
        ("standby",       po::bool_switch()->default_value(false),
                          "F4: start in passive standby mode — receives data but emits no signals; "
                          "promote to active with SIGUSR1")
        ("signal-port",   po::value<uint16_t>()->default_value(0),
                          "ES2: outbound WebSocket port for exec signal push (0 = disabled)")
        ("venue-fees",    po::value<std::string>()->default_value(""),
                          "Per-venue taker fee in bps, comma-separated venue:fee pairs "
                          "(e.g. binance:10,okx:8,bybit:10); used to compute fee-adjusted "
                          "net spread — min-spread-bps is applied to net spread");

    po::variables_map vm;
    try {
        // F2: CLI first, then config file; CLI values win (first store wins).
        po::store(po::parse_command_line(argc, argv, desc), vm);

        const std::string config_path = vm.count("config") ? vm["config"].as<std::string>() : "";
        if (!config_path.empty()) {
            std::ifstream cfg_stream(config_path);
            if (!cfg_stream) {
                std::cerr << "[brain] Error: cannot open config file: " << config_path << "\n";
                return false;
            }
            po::store(po::parse_config_file(cfg_stream, desc, /*allow_unregistered=*/true), vm);
            std::cerr << "[brain] config file loaded: " << config_path << "\n";
        }

        po::notify(vm);
    } catch (const po::error &e) {
        std::cerr << "[brain] CLI error: " << e.what() << "\n";
        return false;
    }

    if (vm.count("help")) {
        std::cerr << "Usage: brain [options]\n\n" << desc << "\n";
        out.show_help = true;
        return true;
    }

    out.bind          = vm["bind"].as<std::string>();
    out.port          = vm["port"].as<uint16_t>();
    out.min_spread_bps = vm["min-spread-bps"].as<double>();
    out.max_spread_bps = vm["max-spread-bps"].as<double>();
    out.rate_limit_ms  = vm["rate-limit-ms"].as<std::int64_t>();
    out.max_age_ms    = vm["max-age-ms"].as<std::int64_t>();
    out.max_price_deviation_pct = vm["max-price-deviation-pct"].as<double>();
    out.output_max_mb = vm["output-max-mb"].as<std::size_t>();
    out.watchdog_no_cross_sec = vm["watchdog-no-cross-sec"].as<std::int64_t>();
    out.depth         = vm["depth"].as<std::size_t>();
    out.log_level = vm["log-level"].as<std::string>();
    out.health_port = vm["health-port"].as<uint16_t>();
    out.standby     = vm["standby"].as<bool>();
    out.signal_port = vm["signal-port"].as<uint16_t>();
    out.venue_fee_bps = parse_venue_fees(vm["venue-fees"].as<std::string>());
    if (vm.count("certfile"))   out.certfile   = vm["certfile"].as<std::string>();
    if (vm.count("keyfile"))    out.keyfile    = vm["keyfile"].as<std::string>();
    if (vm.count("ca-certfile")) out.ca_certfile = vm["ca-certfile"].as<std::string>();
    if (vm.count("output"))   out.output   = vm["output"].as<std::string>();

    if (out.certfile.empty() || out.keyfile.empty()) {
        std::cerr << "[brain] --certfile and --keyfile are required\n";
        return false;
    }
    if (out.max_spread_bps > 0.0 && out.min_spread_bps > out.max_spread_bps) {
        spdlog::warn("[brain] min-spread-bps ({}) > max-spread-bps ({}) — no crosses will emit",
                     out.min_spread_bps, out.max_spread_bps);
    }

    return true;
}

} // namespace brain
