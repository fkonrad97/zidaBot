#pragma once

#include <boost/program_options.hpp>
#include <cstdint>
#include <iostream>
#include <string>

namespace exec {

struct ExecOptions {
    std::string  venue;                          ///< Required: exchange this instance handles
    std::string  brain_host{"127.0.0.1"};        ///< Brain SignalServer host
    uint16_t     brain_port{0};                  ///< Required: brain --signal-port value
    std::string  certfile;                       ///< Optional: mTLS client cert PEM
    std::string  keyfile;                        ///< Optional: mTLS client private key PEM
    bool         insecure_tls{false};            ///< Skip TLS peer verification (dev only)
    double       target_qty{0.01};               ///< Order size in base asset per leg
    double       position_limit{0.0};            ///< E1: gross notional USD cap (0 = disabled)
    double       max_order_notional{0.0};        ///< E4: per-signal price cap USD (0 = disabled)
    std::int64_t cooldown_ms{0};                 ///< E3: dedup window ms (0 = disabled)
    std::int64_t confirmation_timeout_ms{5000};  ///< EX5: fill deadline ms
    bool         dry_run{true};                  ///< EX9: default-safe mode; never send real venue orders
    bool         arm_live{false};                ///< EX10: explicit acknowledgement before live mode
    std::string  enable_live_venue;              ///< EX10: must match --venue for live mode
    double       live_order_notional_cap{25.0};  ///< EX10: max target_qty * max_order_notional in live mode
    std::string  log_level{"info"};              ///< spdlog level: debug|info|warn|error
    bool         show_help{false};
};

inline bool parse_exec_cmdline(int argc, char **argv, ExecOptions &out) {
    namespace po = boost::program_options;

    po::options_description desc("exec");
    desc.add_options()
        ("help,h",           "Show this help message")
        ("venue",            po::value<std::string>(),
                             "Required: exchange identifier (e.g. binance)")
        ("brain-host",       po::value<std::string>()->default_value("127.0.0.1"),
                             "Brain SignalServer host")
        ("brain-port",       po::value<uint16_t>()->default_value(0),
                             "Required: brain --signal-port value")
        ("certfile",         po::value<std::string>(),
                             "mTLS client certificate PEM file")
        ("keyfile",          po::value<std::string>(),
                             "mTLS client private key PEM file")
        ("insecure-tls",     po::bool_switch()->default_value(false),
                             "Skip TLS peer certificate verification (local dev only)")
        ("target-qty",       po::value<double>()->default_value(0.01),
                             "Order size in base asset per leg (default 0.01)")
        ("position-limit",   po::value<double>()->default_value(0.0),
                             "E1: cumulative gross notional USD cap (0 = disabled)")
        ("max-order-notional", po::value<double>()->default_value(0.0),
                             "E4: per-signal price fat-finger cap USD (0 = disabled)")
        ("cooldown-ms",      po::value<std::int64_t>()->default_value(0),
                             "E3: minimum ms between signals for same venue pair (0 = disabled)")
        ("confirmation-timeout-ms", po::value<std::int64_t>()->default_value(5000),
                             "EX5: ms to wait for fill confirmation before timeout (default 5000)")
        ("dry-run",          po::bool_switch()->default_value(false),
                             "EX9: explicit safe mode; log orders and synthesize fills locally")
        ("live-mode",        po::bool_switch()->default_value(false),
                             "EX10: disable default dry-run and require all live rollout guards")
        ("arm-live",         po::bool_switch()->default_value(false),
                             "EX10: required when disabling --dry-run for live venue rollout")
        ("enable-live-venue", po::value<std::string>()->default_value(""),
                             "EX10: venue gate for live mode; must exactly match --venue")
        ("live-order-notional-cap", po::value<double>()->default_value(25.0),
                             "EX10: max target_qty * max-order-notional allowed in live mode (default 25 USD)")
        ("log-level",        po::value<std::string>()->default_value("info"),
                             "Log verbosity: debug | info | warn | error");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const po::error &e) {
        std::cerr << "[exec] CLI error: " << e.what() << "\n";
        return false;
    }

    if (vm.count("help")) {
        std::cerr << "Usage: exec [options]\n\n" << desc << "\n";
        out.show_help = true;
        return true;
    }

    if (!vm.count("venue") || vm["venue"].as<std::string>().empty()) {
        std::cerr << "[exec] --venue is required\n";
        return false;
    }
    if (vm["brain-port"].as<uint16_t>() == 0) {
        std::cerr << "[exec] --brain-port is required\n";
        return false;
    }

    out.venue              = vm["venue"].as<std::string>();
    out.brain_host         = vm["brain-host"].as<std::string>();
    out.brain_port         = vm["brain-port"].as<uint16_t>();
    out.insecure_tls       = vm["insecure-tls"].as<bool>();
    out.target_qty         = vm["target-qty"].as<double>();
    out.position_limit     = vm["position-limit"].as<double>();
    out.max_order_notional = vm["max-order-notional"].as<double>();
    out.cooldown_ms        = vm["cooldown-ms"].as<std::int64_t>();
    out.confirmation_timeout_ms = vm["confirmation-timeout-ms"].as<std::int64_t>();
    const bool dry_run_flag = vm["dry-run"].as<bool>();
    const bool live_mode_flag = vm["live-mode"].as<bool>();
    if (dry_run_flag && live_mode_flag) {
        std::cerr << "[exec] choose either --dry-run or --live-mode, not both\n";
        return false;
    }
    // Safe-by-default startup model:
    //   no mode flag      -> dry-run
    //   --dry-run         -> dry-run
    //   --live-mode       -> live mode, but only if all rollout guards pass below
    out.dry_run            = !live_mode_flag;
    out.arm_live           = vm["arm-live"].as<bool>();
    out.enable_live_venue  = vm["enable-live-venue"].as<std::string>();
    out.live_order_notional_cap = vm["live-order-notional-cap"].as<double>();
    out.log_level          = vm["log-level"].as<std::string>();
    if (vm.count("certfile")) out.certfile = vm["certfile"].as<std::string>();
    if (vm.count("keyfile"))  out.keyfile  = vm["keyfile"].as<std::string>();

    if (out.live_order_notional_cap <= 0.0) {
        std::cerr << "[exec] --live-order-notional-cap must be > 0\n";
        return false;
    }

    if (!out.dry_run) {
        // Live mode is intentionally awkward to enable: the process must be
        // explicitly armed, restricted to one named venue, and bounded by a
        // small startup notional budget so adapter bring-up cannot quietly
        // jump from simulation into broad or oversized live execution.
        if (!out.arm_live) {
            std::cerr << "[exec] refusing live mode without --arm-live; use --dry-run for safe validation\n";
            return false;
        }
        if (out.enable_live_venue != out.venue) {
            std::cerr << "[exec] --enable-live-venue must exactly match --venue in live mode\n";
            return false;
        }
        if (out.max_order_notional <= 0.0) {
            std::cerr << "[exec] live mode requires --max-order-notional to bound order size\n";
            return false;
        }
        const double estimated_order_notional = out.target_qty * out.max_order_notional;
        if (estimated_order_notional > out.live_order_notional_cap) {
            std::cerr << "[exec] live order clamp: target-qty * max-order-notional = "
                      << estimated_order_notional
                      << " exceeds cap " << out.live_order_notional_cap << "\n";
            return false;
        }
    }

    return true;
}

} // namespace exec
