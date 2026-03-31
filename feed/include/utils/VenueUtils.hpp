#pragma once

#include "../abstract/FeedHandler.hpp"
#include <boost/asio/io_context.hpp>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <boost/algorithm/string.hpp>

using json = nlohmann::json;

namespace md {
    namespace venue {
        // --- 2) Symbol mapping ---
        inline std::string map_ws_symbol(VenueId venue,
                                         const std::string &base,
                                         const std::string &quote) {
            // Normalize to UPPER once
            const std::string base_up = boost::algorithm::to_upper_copy(base);
            const std::string quote_up = boost::algorithm::to_upper_copy(quote);

            const std::string concat = base_up + quote_up; // "BTCUSDT"
            const std::string dashed = base_up + "-" + quote_up; // "BTC-USDT"

            switch (venue) {
                case VenueId::BINANCE:
                    // Binance WS paths expect lowercase "btcusdt"
                    return boost::algorithm::to_lower_copy(concat);

                case VenueId::OKX:
                    // OKX uses "BTC-USDT"
                    return dashed;

                case VenueId::BYBIT:
                    // Bybit uses "BTCUSDT" in topics
                    return concat;

                case VenueId::BITGET:
                    // Bitget also uses "BTCUSDT"
                    return concat;

                case VenueId::KUCOIN:
                    // KuCoin uses "BTC-USDT" in topics
                    return dashed;

                default:
                    throw std::invalid_argument("map_ws_symbol: unknown VenueId");
            }
        }

        inline std::string map_rest_symbol(VenueId venue,
                                           const std::string &base,
                                           const std::string &quote) {
            // Normalize to UPPER once
            const std::string base_up = boost::algorithm::to_upper_copy(base);
            const std::string quote_up = boost::algorithm::to_upper_copy(quote);

            const std::string concat = base_up + quote_up; // "BTCUSDT"
            const std::string dashed = base_up + "-" + quote_up; // "BTC-USDT"

            switch (venue) {
                case VenueId::BINANCE:
                    /// Binance WS paths expect lowercase "btcusdt"
                    return boost::algorithm::to_upper_copy(concat);

                case VenueId::OKX:
                    /// OKX REST uses instId like "BTC-USDT"
                    return dashed;

                case VenueId::BITGET:
                    /// Bitget REST uses instId like "BTC-USDT"
                    return dashed;

                case VenueId::BYBIT:
                    return concat;

                case VenueId::KUCOIN:
                    return dashed;

                default:
                    throw std::invalid_argument("map_rest_symbol: unknown VenueId");
            }
        }
    }

    static bool json_to_u64_flexible(const json &jv, std::uint64_t &out) noexcept {
        try {
            if (jv.is_number_unsigned()) {
                out = jv.get<std::uint64_t>();
                return true;
            }
            if (jv.is_number_integer()) {
                auto v = jv.get<std::int64_t>();
                if (v < 0) return false;
                out = (std::uint64_t) v;
                return true;
            }
            if (jv.is_string()) {
                out = std::stoull(jv.get<std::string>());
                return true;
            }
            return false;
        } catch (...) { return false; }
    }

    static bool json_to_i64_flexible(const json &jv, std::int64_t &out) noexcept {
        try {
            if (jv.is_number_integer()) {
                out = jv.get<std::int64_t>();
                return true;
            }
            if (jv.is_number_unsigned()) {
                out = static_cast<std::int64_t>(jv.get<std::uint64_t>());
                return true;
            }
            if (jv.is_string()) {
                out = std::stoll(jv.get<std::string>());
                return true;
            }
            return false;
        } catch (...) { return false; }
    }
} // namespace md
