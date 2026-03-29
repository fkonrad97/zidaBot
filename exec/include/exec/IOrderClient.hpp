#pragma once

#include <string>
#include <system_error>
#include <cstdint>
#include <functional>

namespace exec
{
    enum class Side
    {
        BUY,
        SELL
    };

    /// A single order submitted to an exchange
    struct Order
    {
        std::string venue;
        Side side;
        std::int64_t price_tick; ///< price * 100 (same units as ArbCross)
        double quantity;         ///< base currency quantity
        std::string client_order_id;
    };

    /// Fill confirmation returned by the exchange (or stub)
    struct Fill
    {
        std::string client_order_id;
        double filled_qty;
        std::int64_t fill_price_tick;
        std::int64_t ts_ns;
    };

    /// Pure interface for order submission. Concrete implementations:
    ///   StubOrderClient  — logs + immediate synthetic fill (no network I/O)
    ///   BinanceOrderClient, OkxOrderClient, ... — real REST adapters (future)
    ///
    /// All methods are async — they return immediately and fire the callback
    /// when the exchange confirms. Must be called from the exec Asio strand.
    class IOrderClient
    {
    public:
        virtual ~IOrderClient() = default;

        virtual void submit_order(const Order &order, std::function<void(std::error_code, Fill)> cb) = 0;

        virtual void cancel_order(const std::string &client_order_id, std::function<void(std::error_code)> cb) = 0;
    };
} // namespace exec