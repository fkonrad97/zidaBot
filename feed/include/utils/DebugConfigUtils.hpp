#pragma once

#include <atomic>
#include <cstddef>
#include <spdlog/spdlog.h>
#include <vector>
#include <string_view>
#include "orderbook/OrderBook.hpp"

namespace md::debug {
    inline std::atomic<bool> enabled{false}; // master switch
    inline std::atomic<bool> raw{false}; // print truncated raw msg
    inline std::atomic<int> every{200}; // log 1/N parsed messages
    inline std::atomic<int> raw_max{512}; // truncate raw output
    inline std::atomic<int> top_levels{3}; // print top-of-book levels
    inline std::atomic<bool> show_checksum{true}; // print checksum fields
    inline std::atomic<bool> show_seq{true}; // print seq/prevSeqId

    static bool dbg_on() noexcept {
        return md::debug::enabled.load(std::memory_order_relaxed);
    }

    static bool dbg_sample(std::uint64_t &counter) noexcept {
        const int every = md::debug::every.load(std::memory_order_relaxed);
        return every > 0 && (++counter % static_cast<std::uint64_t>(every) == 0);
    }

    static void dbg_raw(std::string_view msg) {
        if (!md::debug::raw.load(std::memory_order_relaxed)) return;
        int maxc = md::debug::raw_max.load(std::memory_order_relaxed);
        if (maxc <= 0) return;
        if (static_cast<int>(msg.size()) > maxc) msg = msg.substr(0, static_cast<std::size_t>(maxc));
        spdlog::debug("  raw=\"{}\"", msg);
    }

    static void dbg_levels(const char *side, const std::vector<Level> &v) {
        const int top = md::debug::top_levels.load(std::memory_order_relaxed);
        if (top <= 0) return;
        spdlog::debug("  {} top{}:", side, top);
        for (int i = 0; i < top && i < static_cast<int>(v.size()); ++i) {
            spdlog::debug("    {} {} x {}", i, v[i].price, v[i].quantity);
        }
    }
}
