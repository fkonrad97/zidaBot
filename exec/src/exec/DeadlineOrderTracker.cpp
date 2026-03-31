#include "exec/DeadlineOrderTracker.hpp"

#include <spdlog/spdlog.h>

namespace exec {

DeadlineOrderTracker::DeadlineOrderTracker(
    boost::asio::strand<boost::asio::io_context::executor_type> strand,
    std::chrono::milliseconds confirmation_timeout,
    TimeoutCb on_timeout)
    : strand_(strand)
    , timeout_(confirmation_timeout)
    , on_timeout_(std::move(on_timeout)) {}

std::shared_ptr<DeadlineOrderTracker> DeadlineOrderTracker::create(
    boost::asio::strand<boost::asio::io_context::executor_type> strand,
    std::chrono::milliseconds confirmation_timeout,
    TimeoutCb on_timeout)
{
    return std::shared_ptr<DeadlineOrderTracker>(
        new DeadlineOrderTracker(strand, confirmation_timeout, std::move(on_timeout)));
}

void DeadlineOrderTracker::register_pending(const Order &order) {
    auto timer = std::make_unique<boost::asio::steady_timer>(strand_, timeout_);

    timer->async_wait(
        [self = shared_from_this(), oid = order.client_order_id]
        (boost::system::error_code ec)
        {
            if (ec) return; // cancelled — fill arrived before deadline
            spdlog::warn("[OrderTracker] TIMEOUT oid={} — no fill within deadline", oid);
            self->pending_.erase(oid);
            if (self->on_timeout_) self->on_timeout_(oid);
        });

    pending_.emplace(order.client_order_id,
                     PendingEntry{order, std::move(timer)});

    spdlog::debug("[OrderTracker] pending oid={} venue={} side={} qty={} price_tick={}",
                  order.client_order_id, order.venue,
                  order.side == Side::SELL ? "SELL" : "BUY",
                  order.quantity, order.price_tick);
}

void DeadlineOrderTracker::on_fill(const Fill &fill) {
    auto it = pending_.find(fill.client_order_id);
    if (it == pending_.end()) {
        spdlog::warn("[OrderTracker] on_fill: unknown oid={}", fill.client_order_id);
        return;
    }
    it->second.timer->cancel(); // posts operation_aborted to the async_wait handler
    pending_.erase(it);
    spdlog::debug("[OrderTracker] confirmed oid={} qty={} price_tick={}",
                  fill.client_order_id, fill.filled_qty, fill.fill_price_tick);
}

void DeadlineOrderTracker::cancel_all() {
    const std::size_t n = pending_.size();
    for (auto &[oid, entry] : pending_)
        entry.timer->cancel();
    pending_.clear();
    spdlog::info("[OrderTracker] cancel_all: cleared {} pending orders", n);
}

} // namespace exec