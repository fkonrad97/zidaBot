#include <gtest/gtest.h>

#include <chrono>

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

#include "exec/DryRunOrderClient.hpp"

using namespace std::chrono_literals;

TEST(DryRunOrderClient, SubmitOrderPostsSyntheticFillAsynchronously) {
    boost::asio::io_context ioc;
    auto strand = boost::asio::make_strand(ioc);
    exec::DryRunOrderClient client(strand);

    exec::Order order;
    order.venue = "binance";
    order.side = exec::Side::BUY;
    order.price_tick = 50000;
    order.quantity = 0.25;
    order.client_order_id = "dry-1";

    int fired = 0;
    exec::Fill captured{};
    client.submit_order(order, [&](std::error_code ec, exec::Fill fill) {
        EXPECT_FALSE(ec);
        captured = fill;
        ++fired;
    });

    EXPECT_EQ(fired, 0);
    ioc.run_for(20ms);

    EXPECT_EQ(fired, 1);
    EXPECT_EQ(captured.client_order_id, "dry-1");
    EXPECT_DOUBLE_EQ(captured.filled_qty, 0.25);
    EXPECT_EQ(captured.fill_price_tick, 50000);
    EXPECT_GT(captured.ts_ns, 0);
}

TEST(DryRunOrderClient, CancelOrderAcknowledgesAsynchronously) {
    boost::asio::io_context ioc;
    auto strand = boost::asio::make_strand(ioc);
    exec::DryRunOrderClient client(strand);

    int fired = 0;
    client.cancel_order("dry-2", [&](std::error_code ec) {
        EXPECT_FALSE(ec);
        ++fired;
    });

    EXPECT_EQ(fired, 0);
    ioc.run_for(20ms);
    EXPECT_EQ(fired, 1);
}
