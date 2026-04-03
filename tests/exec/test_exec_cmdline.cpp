#include <gtest/gtest.h>

#include "exec/ExecCmdLine.hpp"

TEST(ExecCmdLine, DryRunDefaultsToTrue) {
    exec::ExecOptions opts;
    char arg0[] = "exec";
    char arg1[] = "--venue";
    char arg2[] = "binance";
    char arg3[] = "--brain-port";
    char arg4[] = "9001";
    char *argv[] = {arg0, arg1, arg2, arg3, arg4};

    ASSERT_TRUE(exec::parse_exec_cmdline(5, argv, opts));
    EXPECT_TRUE(opts.dry_run);
}

TEST(ExecCmdLine, DryRunFlagEnablesSafeMode) {
    exec::ExecOptions opts;
    char arg0[] = "exec";
    char arg1[] = "--venue";
    char arg2[] = "binance";
    char arg3[] = "--brain-port";
    char arg4[] = "9001";
    char arg5[] = "--dry-run";
    char *argv[] = {arg0, arg1, arg2, arg3, arg4, arg5};

    ASSERT_TRUE(exec::parse_exec_cmdline(6, argv, opts));
    EXPECT_TRUE(opts.dry_run);
}

TEST(ExecCmdLine, LiveModeRequiresArmLive) {
    exec::ExecOptions opts;
    char arg0[] = "exec";
    char arg1[] = "--venue";
    char arg2[] = "binance";
    char arg3[] = "--brain-port";
    char arg4[] = "9001";
    char arg5[] = "--live-mode";
    char arg6[] = "--max-order-notional";
    char arg7[] = "100";
    char arg8[] = "--enable-live-venue";
    char arg9[] = "binance";
    char *argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9};

    EXPECT_FALSE(exec::parse_exec_cmdline(10, argv, opts));
}

TEST(ExecCmdLine, LiveModeRequiresMatchingVenueGate) {
    exec::ExecOptions opts;
    char arg0[] = "exec";
    char arg1[] = "--venue";
    char arg2[] = "binance";
    char arg3[] = "--brain-port";
    char arg4[] = "9001";
    char arg5[] = "--live-mode";
    char arg6[] = "--arm-live";
    char arg7[] = "--max-order-notional";
    char arg8[] = "100";
    char arg9[] = "--enable-live-venue";
    char arg10[] = "okx";
    char *argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10};

    EXPECT_FALSE(exec::parse_exec_cmdline(11, argv, opts));
}

TEST(ExecCmdLine, LiveModeRequiresMaxOrderNotional) {
    exec::ExecOptions opts;
    char arg0[] = "exec";
    char arg1[] = "--venue";
    char arg2[] = "binance";
    char arg3[] = "--brain-port";
    char arg4[] = "9001";
    char arg5[] = "--live-mode";
    char arg6[] = "--arm-live";
    char arg7[] = "--enable-live-venue";
    char arg8[] = "binance";
    char *argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8};

    EXPECT_FALSE(exec::parse_exec_cmdline(9, argv, opts));
}

TEST(ExecCmdLine, LiveModeRejectsOrdersAboveTinyNotionalClamp) {
    exec::ExecOptions opts;
    char arg0[] = "exec";
    char arg1[] = "--venue";
    char arg2[] = "binance";
    char arg3[] = "--brain-port";
    char arg4[] = "9001";
    char arg5[] = "--live-mode";
    char arg6[] = "--arm-live";
    char arg7[] = "--enable-live-venue";
    char arg8[] = "binance";
    char arg9[] = "--target-qty";
    char arg10[] = "1.0";
    char arg11[] = "--max-order-notional";
    char arg12[] = "100";
    char arg13[] = "--live-order-notional-cap";
    char arg14[] = "25";
    char *argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12, arg13, arg14};

    EXPECT_FALSE(exec::parse_exec_cmdline(15, argv, opts));
}

TEST(ExecCmdLine, LiveModePassesWhenAllGuardsSatisfied) {
    exec::ExecOptions opts;
    char arg0[] = "exec";
    char arg1[] = "--venue";
    char arg2[] = "binance";
    char arg3[] = "--brain-port";
    char arg4[] = "9001";
    char arg5[] = "--live-mode";
    char arg6[] = "--arm-live";
    char arg7[] = "--enable-live-venue";
    char arg8[] = "binance";
    char arg9[] = "--target-qty";
    char arg10[] = "0.1";
    char arg11[] = "--max-order-notional";
    char arg12[] = "100";
    char arg13[] = "--live-order-notional-cap";
    char arg14[] = "25";
    char *argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12, arg13, arg14};

    ASSERT_TRUE(exec::parse_exec_cmdline(15, argv, opts));
    EXPECT_FALSE(opts.dry_run);
    EXPECT_TRUE(opts.arm_live);
    EXPECT_EQ(opts.enable_live_venue, "binance");
    EXPECT_DOUBLE_EQ(opts.live_order_notional_cap, 25.0);
}
