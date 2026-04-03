#include "exec/ImmediateStrategy.hpp"
#include "exec/OrderTracker.hpp"

#include <spdlog/spdlog.h>

namespace exec {

ImmediateStrategy::ImmediateStrategy(
    std::string            my_venue,
    IOrderClient          &client,
    OrderTracker          &tracker,
    std::function<void(const Fill &)> on_fill,
    boost::asio::strand<boost::asio::io_context::executor_type> strand,
    double                 target_qty)
    : my_venue_(std::move(my_venue))
    , client_(client)
    , tracker_(tracker)
    , on_fill_(std::move(on_fill))
    , strand_(strand)
    , target_qty_(target_qty) {}

void ImmediateStrategy::on_signal(const brain::ArbCross &cross) {
    if (paused_.load(std::memory_order_relaxed)) return;

    // Determine which leg this venue handles.
    Side side;
    std::int64_t price_tick;
    double       level_qty;

    if (cross.sell_venue == my_venue_) {
        side       = Side::SELL;
        price_tick = cross.sell_bid_tick;
        level_qty  = 0.0; // filled from order book depth in future; use target_qty for now
    } else if (cross.buy_venue == my_venue_) {
        side       = Side::BUY;
        price_tick = cross.buy_ask_tick;
        level_qty  = 0.0;
    } else {
        return; // signal not for this venue
    }

    const double qty = (target_qty_ > 0.0) ? target_qty_ : level_qty;
    if (qty <= 0.0) {
        spdlog::warn("[ImmediateStrategy] qty=0 for venue={} — skipping", my_venue_);
        return;
    }

    Order order;
    order.venue            = my_venue_;
    order.side             = side;
    order.price_tick       = price_tick;
    order.quantity         = qty;
    order.client_order_id  = next_order_id_();

    spdlog::info("[ImmediateStrategy] {} {} qty={} price_tick={} id={}",
                 my_venue_,
                 side == Side::SELL ? "SELL" : "BUY",
                 qty, price_tick,
                 order.client_order_id);

    tracker_.register_pending(order);

    client_.submit_order(order,
        [this, oid = order.client_order_id](std::error_code ec, Fill fill) {
            if (ec) {
                spdlog::warn("[ImmediateStrategy] submit failed id={} err={}", oid, ec.message());
                return;
            }
            spdlog::info("[ImmediateStrategy] FILL id={} qty={} price_tick={}",
                         fill.client_order_id, fill.filled_qty, fill.fill_price_tick);
            // A confirmed fill has two separate side effects:
            //   1. clear E5 timeout tracking for this order
            //   2. feed E1 position accounting in ExecEngine via on_fill_
            tracker_.on_fill(fill);
            if (on_fill_) on_fill_(fill);
        });
}

std::string ImmediateStrategy::next_order_id_() {
    return my_venue_ + "-" + std::to_string(++order_seq_);
}

} // namespace exec
