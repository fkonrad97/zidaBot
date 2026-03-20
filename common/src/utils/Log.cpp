#include "utils/Log.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <vector>

namespace md::log {

    void init(const std::string &level_str, const std::string &file_path) {
        std::vector<spdlog::sink_ptr> sinks;

        // Stderr sink — always present, with ANSI colour on TTYs.
        auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>(); // in stdout_color_sinks.h
        sinks.push_back(stderr_sink);

        // Optional file sink — appends to an existing file on process restart.
        if (!file_path.empty()) {
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                file_path, /*truncate=*/false);
            sinks.push_back(file_sink);
        }

        auto logger = std::make_shared<spdlog::logger>(
            "pop", sinks.begin(), sinks.end());

        spdlog::set_default_logger(logger);

        // Parse level string; falls back to "info" on unknown strings.
        spdlog::set_level(spdlog::level::from_str(level_str));

        // Format: [2026-03-20 12:34:56.789] [info] message
        // %^ / %$ wrap the level name in colour codes on colour-capable sinks.
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

        // Flush immediately on warn+ so crash logs are not lost in a buffer.
        spdlog::flush_on(spdlog::level::warn);
    }

    void flush() {
        spdlog::default_logger()->flush();
    }

} // namespace md::log
