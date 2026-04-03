#pragma once

#include <chrono>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <spdlog/spdlog.h>

#include "exec/IOrderClient.hpp"

namespace exec {

/// Production-facing dry-run client for safe venue validation.
/// It never touches an exchange: orders are logged and acknowledged with a
/// synthetic full fill posted asynchronously onto the exec strand.
///
/// This is intentionally "production-shaped": the rest of the exec stack sees
/// the same async callback pattern it would get from a real venue adapter, so
/// rollout checks can exercise E1/E5 and strategy wiring without venue risk.
class DryRunOrderClient : public IOrderClient {
public:
    explicit DryRunOrderClient(
        boost::asio::strand<boost::asio::io_context::executor_type> strand)
        : strand_(std::move(strand)) {}

    void submit_order(const Order &order,
                      std::function<void(std::error_code, Fill)> cb) override {
        spdlog::info("[DRY-RUN] SUBMIT venue={} side={} qty={} price_tick={} id={}",
                     order.venue,
                     order.side == Side::BUY ? "BUY" : "SELL",
                     order.quantity,
                     order.price_tick,
                     order.client_order_id);

        Fill fill;
        fill.client_order_id = order.client_order_id;
        fill.filled_qty = order.quantity;
        fill.fill_price_tick = order.price_tick;
        fill.ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

        // Keep the callback asynchronous so dry-run preserves the same control
        // flow assumptions as a real network-backed order client.
        boost::asio::post(strand_, [cb = std::move(cb), fill]() mutable { cb({}, fill); });
    }

    void cancel_order(const std::string &client_order_id,
                      std::function<void(std::error_code)> cb) override {
        spdlog::info("[DRY-RUN] CANCEL id={}", client_order_id);
        boost::asio::post(strand_, [cb = std::move(cb)]() mutable { cb({}); });
    }

private:
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
};

} // namespace exec
