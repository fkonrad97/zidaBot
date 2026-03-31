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
    out.log_level          = vm["log-level"].as<std::string>();
    if (vm.count("certfile")) out.certfile = vm["certfile"].as<std::string>();
    if (vm.count("keyfile"))  out.keyfile  = vm["keyfile"].as<std::string>();

    return true;
}

} // namespace exec
