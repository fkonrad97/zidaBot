#pragma once

#include <atomic>
#include <string>

#include <boost/asio/strand.hpp>
#include <boost/asio/io_context.hpp>

#include "exec/IExecStrategy.hpp"
#include "exec/IOrderClient.hpp"

namespace exec {

class OrderTracker;

/// Simplest execution strategy: one market order per leg per signal.
/// Determines BUY or SELL leg from my_venue_, sizes from target_qty
/// (or full level-0 quantity when target_qty == 0), submits immediately.
class ImmediateStrategy : public IExecStrategy {
public:
    /// @param my_venue     venue this exec instance is responsible for
    /// @param client       order submission interface (stub or real)
    /// @param tracker      E5: registers pending orders for confirmation tracking
    /// @param strand       Asio strand — all async callbacks run here
    /// @param target_qty   max qty per leg in base currency (0 = full level-0)
    ImmediateStrategy(std::string            my_venue,
                      IOrderClient          &client,
                      OrderTracker          &tracker,
                      boost::asio::strand<boost::asio::io_context::executor_type> &strand,
                      double                 target_qty = 0.0);

    void on_signal(const brain::ArbCross &cross) override;
    void pause()  override { paused_.store(true,  std::memory_order_relaxed); }
    void resume() override { paused_.store(false, std::memory_order_relaxed); }

private:
    /// Generate a unique client_order_id for each submitted order.
    std::string next_order_id_();

    std::string    my_venue_;
    IOrderClient  &client_;
    OrderTracker  &tracker_;
    boost::asio::strand<boost::asio::io_context::executor_type> &strand_;
    double         target_qty_;
    std::atomic<bool> paused_{false};
    std::uint64_t  order_seq_{0};   ///< monotonic counter for client_order_id generation
};

} // namespace exec