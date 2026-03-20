#pragma once

// D1: spdlog-backed levelled logger facade.
// All source files that log should include this header and call
// spdlog::info/warn/error/debug directly with fmt-style {} placeholders.
//
// Log levels (passed to md::log::init):
//   "debug" — per-update bridge/sequence details; very verbose on live venues
//   "info"  — state transitions, heartbeats, connection events (default)
//   "warn"  — anomalies: resyncs, book crossed, checksum mismatch, watchdog
//   "error" — fatal/unrecoverable: TLS failure, file open failure, bad config
//
// Sinks:
//   - stderr always (with colour on terminals)
//   - file if a non-empty file_path is passed to init()

#include <spdlog/spdlog.h>
#include <string>

namespace md::log {
    /// Initialise the global spdlog default logger.
    /// Call once in main() before any other logging.
    /// level_str: "debug" | "info" | "warn" | "error"  (default "info")
    /// file_path: if non-empty, also write to this file (appended, not truncated)
    void init(const std::string &level_str = "info",
              const std::string &file_path = "");

    /// Flush all sinks.  Call before process exit or on signal.
    void flush();
} // namespace md::log
