#pragma once

#include <chrono>
#include <spdlog/spdlog.h>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include "exec/IOrderClient.hpp"

namespace exec
{

    /// Stub implementation of IOrderClient for development and testing.
    /// Logs the order intent via spdlog, then posts the callback onto a strand
    /// to fire asynchronously — matching the contract of real async clients.
    /// No network I/O — safe to use without exchange connectivity.
    class StubOrderClient : public IOrderClient
    {
    public:
        /// @param strand  Asio strand onto which callbacks are posted.
        explicit StubOrderClient(
            boost::asio::strand<boost::asio::io_context::executor_type> strand)
            : strand_(std::move(strand)) {}

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

            // Post asynchronously onto the strand to match real client contract.
            boost::asio::post(strand_,
                              [cb = std::move(cb), fill]() mutable
                              { cb({}, fill); }); // empty error_code = success
        }

        void cancel_order(const std::string &client_order_id,
                          std::function<void(std::error_code)> cb) override
        {
            spdlog::info("[EXEC] CANCEL id={}", client_order_id);
            boost::asio::post(strand_,
                              [cb = std::move(cb)]() mutable
                              { cb({}); }); // empty error_code = success
        }

    private:
        boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    };

} // namespace exec