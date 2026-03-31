#pragma once

#include <string>
#include "exec/IOrderClient.hpp"

namespace exec {

/// E5: tracks pending orders and arms a confirmation deadline timer.
/// Full implementation in EX5. Forward-declared here so ImmediateStrategy
/// can call register_pending() without the full Asio timer machinery.
class OrderTracker {
public:
    /// Register a submitted order for confirmation tracking.
    /// Called immediately after IOrderClient::submit_order() fires its callback.
    virtual void register_pending(const Order &order) = 0;

    /// Mark an order as confirmed. Called from the fill callback.
    virtual void on_fill(const Fill &fill) = 0;

    virtual ~OrderTracker() = default;
};

} // namespace exec