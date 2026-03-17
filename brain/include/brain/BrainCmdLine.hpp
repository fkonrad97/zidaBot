#pragma once

#include <boost/program_options.hpp>
#include <cstdint>
#include <iostream>
#include <string>

namespace brain {

struct BrainOptions {
    std::string bind{"0.0.0.0"};
    uint16_t    port{8443};
    std::string certfile;       ///< Required: TLS cert PEM to present to PoP clients
    std::string keyfile;        ///< Required: TLS private key PEM
    std::string output;         ///< Optional: arb signal JSONL output file path
    double      min_spread_bps{0.0};
    std::int64_t max_age_ms{5000};
    std::size_t depth{50};      ///< OrderBook depth per venue
    bool        show_help{false};
};

inline bool parse_brain_cmdline(int argc, char **argv, BrainOptions &out) {
    namespace po = boost::program_options;

    po::options_description desc("brain");
    desc.add_options()
        ("help,h", "Show this help message")
        ("bind",          po::value<std::string>()->default_value("0.0.0.0"),
                          "Bind address")
        ("port",          po::value<uint16_t>()->default_value(8443),
                          "Listen port (WSS)")
        ("certfile",      po::value<std::string>(),
                          "TLS certificate PEM file")
        ("keyfile",       po::value<std::string>(),
                          "TLS private key PEM file")
        ("output",        po::value<std::string>(),
                          "Arb signal output JSONL file path")
        ("min-spread-bps",po::value<double>()->default_value(0.0),
                          "Minimum spread in bps to emit (default 0 = all crosses)")
        ("max-age-ms",    po::value<std::int64_t>()->default_value(5000),
                          "Max book age difference ms for cross-venue staleness guard")
        ("depth",         po::value<std::size_t>()->default_value(50),
                          "OrderBook depth per venue");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
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
    out.max_age_ms    = vm["max-age-ms"].as<std::int64_t>();
    out.depth         = vm["depth"].as<std::size_t>();
    if (vm.count("certfile")) out.certfile = vm["certfile"].as<std::string>();
    if (vm.count("keyfile"))  out.keyfile  = vm["keyfile"].as<std::string>();
    if (vm.count("output"))   out.output   = vm["output"].as<std::string>();

    if (out.certfile.empty() || out.keyfile.empty()) {
        std::cerr << "[brain] --certfile and --keyfile are required\n";
        return false;
    }

    return true;
}

} // namespace brain
