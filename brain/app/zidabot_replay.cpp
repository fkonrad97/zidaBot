/// zidabot_replay — Replay historical PoP JSONL data through the C++ arb engine.
///
/// Reads raw JSONL lines from stdin (one event per line), runs each through
/// BacktestEngine, and writes to stdout as JSON lines:
///
///   Without --emit-books (default):
///     One line per detected ArbCross, {"type":"cross", ...}
///
///   With --emit-books:
///     One {"type":"book", ...} line per event (showing the updated venue's
///     full depth curve), plus {"type":"cross", ...} lines on arb detections.
///
/// Usage:
///   zcat binance.jsonl.gz okx.jsonl.gz | ./build/brain/zidabot_replay [options]

#include <iostream>
#include <string>

#include <boost/program_options.hpp>
#include <nlohmann/json.hpp>

#include "brain/BacktestEngine.hpp"
#include "utils/Log.hpp"

namespace po = boost::program_options;
using namespace brain;

int main(int argc, char *argv[]) {
    std::size_t  depth                   = 50;
    double       min_spread_bps          = 0.0;
    double       max_spread_bps          = 0.0;
    std::int64_t rate_limit_ms           = 0;
    std::int64_t max_age_ms              = 5000;
    double       max_price_deviation_pct = 0.0;
    bool         emit_books              = false;
    std::size_t  book_depth              = 0;   // 0 = full configured depth
    std::string  log_level               = "warn"; // default: suppress info noise on stdout

    try {
        po::options_description desc("zidabot_replay options");
        desc.add_options()
            ("help,h",               "Show help")
            ("depth",                po::value<std::size_t>(&depth)->default_value(50),
                                     "Order book depth per venue")
            ("min-spread-bps",       po::value<double>(&min_spread_bps)->default_value(0.0),
                                     "Suppress crosses below this spread (bps)")
            ("max-spread-bps",       po::value<double>(&max_spread_bps)->default_value(0.0),
                                     "Suppress crosses above this spread, 0=no cap (bps)")
            ("rate-limit-ms",        po::value<std::int64_t>(&rate_limit_ms)->default_value(0),
                                     "Min ms between signals per venue pair, 0=unlimited")
            ("max-age-ms",           po::value<std::int64_t>(&max_age_ms)->default_value(5000),
                                     "Max book age for arb scan (ms)")
            ("max-price-deviation-pct", po::value<double>(&max_price_deviation_pct)->default_value(0.0),
                                     "B5: exclude venue if price deviates from median by this %, 0=off")
            ("emit-books",           po::bool_switch(&emit_books),
                                     "Emit a {\"type\":\"book\"} line after every event (depth curve)")
            ("book-depth",           po::value<std::size_t>(&book_depth)->default_value(0),
                                     "Levels per side in book output, 0=full depth (only with --emit-books)")
            ("log-level",            po::value<std::string>(&log_level)->default_value("warn"),
                                     "Log verbosity sent to stderr: debug|info|warn|error");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            return 0;
        }
    } catch (const std::exception &e) {
        std::cerr << "argument error: " << e.what() << "\n";
        return 1;
    }

    // Route all spdlog output to stderr so stdout carries only JSON lines.
    md::log::init(log_level, /*file_path=*/"");

    const std::int64_t rate_limit_ns = rate_limit_ms * 1'000'000LL;
    const std::int64_t max_age_ns    = max_age_ms    * 1'000'000LL;

    BacktestEngine engine(depth, min_spread_bps, max_spread_bps,
                          rate_limit_ns, max_age_ns, max_price_deviation_pct);

    // Disable stdout buffering so Python subprocess.stdout.readline() doesn't block.
    std::cout << std::unitbuf;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        const auto crosses = engine.feed_event(line);
        const auto &venue  = engine.last_updated_venue();

        // Depth curve output (one line per event for the updated venue).
        if (emit_books && !venue.empty()) {
            const auto bd = engine.levels(venue, book_depth);
            if (!bd.bids.empty() || !bd.asks.empty()) {
                nlohmann::json j;
                j["type"]    = "book";
                j["venue"]   = bd.venue;
                j["ts_ns"]   = bd.ts_ns;
                auto &bids_j = j["bids"];
                for (const auto &l : bd.bids)
                    bids_j.push_back({l.price_tick, l.qty_lot});
                auto &asks_j = j["asks"];
                for (const auto &l : bd.asks)
                    asks_j.push_back({l.price_tick, l.qty_lot});
                std::cout << j.dump() << "\n";
            }
        }

        // Cross output.
        for (const auto &cross : crosses) {
            nlohmann::json j;
            j["type"]            = "cross";
            j["sell_venue"]      = cross.sell_venue;
            j["buy_venue"]       = cross.buy_venue;
            j["sell_bid_tick"]   = cross.sell_bid_tick;
            j["buy_ask_tick"]    = cross.buy_ask_tick;
            j["spread_bps"]      = cross.spread_bps;
            j["ts_detected_ns"]  = cross.ts_detected_ns;
            j["sell_ts_book_ns"] = cross.sell_ts_book_ns;
            j["buy_ts_book_ns"]  = cross.buy_ts_book_ns;
            j["lag_ns"]          = cross.ts_detected_ns
                                   - std::max(cross.sell_ts_book_ns, cross.buy_ts_book_ns);
            std::cout << j.dump() << "\n";
        }
    }

    return 0;
}
