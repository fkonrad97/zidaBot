#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace brain {

/// Fixed-size circular buffer of nanosecond latency samples.
/// Computes p50/p95/p99 on demand via full sort (O(n log n), n <= 10 000).
class LatencyHistogram {
public:
    void record(std::int64_t ns) noexcept {
        buf_[pos_] = ns;
        pos_ = (pos_ + 1) % kCap;
        if (count_ < kCap) ++count_;
    }

    struct Percentiles {
        std::int64_t p50_us{0};
        std::int64_t p95_us{0};
        std::int64_t p99_us{0};
        std::size_t  n{0};
    };

    /// Returns percentiles in microseconds. Copies + sorts the buffer (rare path).
    [[nodiscard]] Percentiles compute() const {
        if (count_ == 0) return {};
        std::vector<std::int64_t> v(buf_.begin(), buf_.begin() + count_);
        std::sort(v.begin(), v.end());
        const auto pct = [&](double p) -> std::int64_t {
            const std::size_t idx =
                static_cast<std::size_t>(p / 100.0 * static_cast<double>(v.size() - 1));
            return v[idx] / 1'000; // ns → us
        };
        return {pct(50.0), pct(95.0), pct(99.0), count_};
    }

    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

private:
    static constexpr std::size_t kCap = 10'000;
    std::array<std::int64_t, kCap> buf_{};
    std::size_t pos_{0};
    std::size_t count_{0}; ///< saturates at kCap
};

} // namespace brain
