#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace md {

/// Parse a decimal string into integer ticks by scaling in integer arithmetic.
/// scale=100  → priceTick (price * 100, 2 decimal places kept)
/// scale=1000 → quantityLot (qty * 1000, 3 decimal places kept)
///
/// Avoids the double intermediate used by stod(), which loses precision above
/// ~9e13 (≈ price*scale > 2^53). With int64_t the safe range is ~9e16 ticks
/// (price < 9e14 at scale=100), which covers all foreseeable crypto prices.
///
/// Throws std::invalid_argument on malformed input.
inline std::int64_t parseDecimalToScaled(std::string_view s, std::int64_t scale) {
    if (s.empty()) throw std::invalid_argument("parseDecimalToScaled: empty string");

    bool negative = false;
    if (s.front() == '-') {
        negative = true;
        s.remove_prefix(1);
    }

    // Split on '.'
    const auto dot = s.find('.');
    const std::string_view int_part  = (dot == std::string_view::npos) ? s          : s.substr(0, dot);
    const std::string_view frac_part = (dot == std::string_view::npos) ? std::string_view{} : s.substr(dot + 1);

    // Parse integer part
    std::int64_t int_val = 0;
    if (!int_part.empty()) {
        const auto [ptr, ec] = std::from_chars(int_part.data(), int_part.data() + int_part.size(), int_val);
        if (ec != std::errc{}) throw std::invalid_argument("parseDecimalToScaled: bad integer part");
    }

    // Accumulate fractional digits up to the required scale length
    std::int64_t frac_val     = 0;
    std::int64_t frac_divisor = 1;
    std::size_t  digits_used  = 0;
    for (char c : frac_part) {
        if (frac_divisor >= scale) break; // sufficient precision reached
        if (c < '0' || c > '9') throw std::invalid_argument("parseDecimalToScaled: bad fractional digit");
        frac_val     = frac_val * 10 + (c - '0');
        frac_divisor *= 10;
        ++digits_used;
    }

    // Pad to full scale (e.g. "1.5" at scale 100 → frac_val=5, divisor=10 → scaled=50)
    while (frac_divisor < scale) {
        frac_val     *= 10;
        frac_divisor *= 10;
    }

    const std::int64_t result = int_val * scale + frac_val;
    return negative ? -result : result;
}

/// Convert a price string to integer ticks (price × 100, 2 decimal places).
inline std::int64_t parsePriceToTicks(const std::string &s) {
    return parseDecimalToScaled(s, 100);
}

/// Convert a quantity string to integer lots (qty × 1000, 3 decimal places).
inline std::int64_t parseQtyToLots(const std::string &s) {
    return parseDecimalToScaled(s, 1000);
}

} // namespace md
