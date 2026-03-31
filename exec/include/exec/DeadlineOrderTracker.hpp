#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include "exec/IOrderClient.hpp"
#include "exec/OrderTracker.hpp"

namespace exec
{

    /// EX5: concrete OrderTracker with per-order Asio steady_timer deadline.
    /// If a fill is not received within confirmation_timeout, the order is
    /// evicted from the pending map and on_timeout_ is called (if set).
    ///
    /// All methods must be called from the exec Asio strand — thread safety
    /// is by strand serialisation, no mutex needed.
    class DeadlineOrderTracker : public OrderTracker,
                                 public std::enable_shared_from_this<DeadlineOrderTracker>
    {
    public:
        using TimeoutCb = std::function<void(const std::string &client_order_id)>;

        static std::shared_ptr<DeadlineOrderTracker> create(
            boost::asio::strand<boost::asio::io_context::executor_type> strand,
            std::chrono::milliseconds confirmation_timeout,
            TimeoutCb on_timeout = nullptr);

        void register_pending(const Order &order) override;
        void on_fill(const Fill &fill) override;

        /// Cancel all outstanding timers. Call before ioc stops.
        void cancel_all();

        /// Number of orders currently awaiting confirmation.
        [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }

    private:
        DeadlineOrderTracker(
            boost::asio::strand<boost::asio::io_context::executor_type> strand,
            std::chrono::milliseconds confirmation_timeout,
            TimeoutCb on_timeout);

        struct PendingEntry
        {
            Order order;
            std::unique_ptr<boost::asio::steady_timer> timer;
        };

        boost::asio::strand<boost::asio::io_context::executor_type> strand_;
        std::chrono::milliseconds timeout_;
        TimeoutCb on_timeout_;
        std::unordered_map<std::string, PendingEntry> pending_;
    };

} // namespace exec