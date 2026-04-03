#include <gtest/gtest.h>

#include "brain/BrainCmdLine.hpp"

using namespace brain;

TEST(BrainCmdLine, ParseVenueFees_EmptyStringReturnsEmptyMap) {
    const auto fees = parse_venue_fees("");
    EXPECT_TRUE(fees.empty());
}

TEST(BrainCmdLine, ParseVenueFees_WhitespaceIsTrimmed) {
    const auto fees = parse_venue_fees(" binance : 10 , okx: 8 ");

    ASSERT_EQ(fees.size(), 2u);
    EXPECT_DOUBLE_EQ(fees.at("binance"), 10.0);
    EXPECT_DOUBLE_EQ(fees.at("okx"), 8.0);
}

TEST(BrainCmdLine, ParseVenueFees_NegativeFeeIsRejected) {
    const auto fees = parse_venue_fees("binance:-5,okx:8");

    ASSERT_EQ(fees.size(), 1u);
    EXPECT_EQ(fees.count("binance"), 0u);
    EXPECT_DOUBLE_EQ(fees.at("okx"), 8.0);
}

TEST(BrainCmdLine, ParseVenueFees_MalformedTokensAreSkipped) {
    const auto fees = parse_venue_fees("binance,okx:, :8,bybit:abc,kucoin:10");

    ASSERT_EQ(fees.size(), 1u);
    EXPECT_DOUBLE_EQ(fees.at("kucoin"), 10.0);
}

TEST(BrainCmdLine, ParseVenueFees_SuspiciouslyLargeFeeIsAccepted) {
    const auto fees = parse_venue_fees("binance:1500");

    ASSERT_EQ(fees.size(), 1u);
    EXPECT_DOUBLE_EQ(fees.at("binance"), 1500.0);
}
