#pragma once

#include <chrono>
#include <spdlog/spdlog.h>
#include <string>

#include "exec/IOrderClient.hpp"

namespace exec
{

    /// Stub implementation of IOrderClient for development and testing.
    /// Logs the order intent via spdlog, then immediately fires the callback
    /// with a synthetic fill at the requested price and full quantity.
    /// No network I/O — safe to use without exchange connectivity.
    class StubOrderClient : public IOrderClient
    {
    public:
        void submit_order(const Order &order,
                          std::function<void(std::error_code, Fill)> cb) override
        {
            spdlog::info("[EXEC] SUBMIT {} {} qty={} price_tick={} id={}",
                         order.venue,
                         order.side == Side::BUY ? "BUY" : "SELL",
                         order.quantity,
                         order.price_tick,
                         order.client_order_id);

            // Synthetic fill: full quantity at requested price, timestamped now.
            Fill fill;
            fill.client_order_id = order.client_order_id;
            fill.filled_qty = order.quantity;
            fill.fill_price_tick = order.price_tick;
            fill.ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

            cb({}, fill); // empty error_code = success
        }

        void cancel_order(const std::string &client_order_id,
                          std::function<void(std::error_code)> cb) override
        {
            spdlog::info("[EXEC] CANCEL id={}", client_order_id);
            cb({}); // empty error_code = success
        }
    };

} // namespace exec