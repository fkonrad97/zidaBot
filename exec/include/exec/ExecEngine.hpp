#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "brain/ArbDetector.hpp"
#include "exec/IExecStrategy.hpp"
#include "exec/IOrderClient.hpp"

namespace exec {

/// EX4: strand-owned orchestrator that sits between the inbound WsClient
/// (ArbCross JSON from brain's SignalServer) and the pluggable IExecStrategy.
///
/// Guard order on every signal: E4 (fat-finger) → E1 (position limit) →
///   E2 (kill switch) → E3 (dedup / cooldown) → strategy_->on_signal().
///
/// All methods except pause() and resume() MUST be called from the exec
/// Asio strand. pause()/resume() store into an atomic and are therefore
/// safe to call from any thread, including a POSIX signal handler — with
/// the caveat that the virtual strategy_->pause()/resume() calls inside
/// those methods are NOT async-signal-safe (flagged for EX6).
class ExecEngine {
public:
    /// @param my_venue          Exchange identifier this engine handles (e.g. "binance").
    /// @param strategy          Pluggable execution strategy; engine takes ownership.
    /// @param position_limit    E1: cumulative gross notional cap in USD. 0 = disabled.
    /// @param max_order_notional E4: per-signal price cap in USD. 0 = disabled.
    /// @param cooldown_ns       E3: minimum nanoseconds between signals for the same
    ///                          directed pair (sell_venue:buy_venue). 0 = disabled.
    ExecEngine(std::string                    my_venue,
               std::unique_ptr<IExecStrategy> strategy,
               double                         position_limit,
               double                         max_order_notional,
               std::int64_t                   cooldown_ns);

    ExecEngine(const ExecEngine &) = delete;
    ExecEngine &operator=(const ExecEngine &) = delete;

    /// Entry point for arb signals. Applies E1–E4 guards then forwards to strategy.
    /// Must be called from the exec Asio strand.
    void on_signal(const brain::ArbCross &cross);

    /// Accumulates filled notional into open_notional_ (E1 tracking).
    /// Called from the strategy fill callback, which runs on the exec strand.
    void on_fill(const Fill &fill);

    /// E2 kill switch — atomically disables signal dispatch.
    /// Safe to call from any thread (atomic store only).
    /// NOTE: also calls strategy_->pause() which is a virtual call — not safe from
    ///       a POSIX signal handler. Production fix: signal handler sets atomic only;
    ///       on_signal() bounces on paused_ check. Tracked as EX6.
    void pause();

    /// Resumes signal dispatch after a pause(). See pause() caveat.
    void resume();

private:
    std::string       my_venue_;
    std::atomic<bool> paused_{false};                               ///< E2 kill switch
    double            open_notional_{0.0};                          ///< E1 gross cumulative notional
    std::unordered_map<std::string, std::int64_t> last_signal_ns_;  ///< E3 dedup map keyed "sell:buy"
    double            position_limit_{0.0};                         ///< E1 cap; 0 = no limit
    double            max_order_notional_{0.0};                     ///< E4 cap; 0 = no limit
    std::int64_t      cooldown_ns_{0};                              ///< E3 cooldown; 0 = no cooldown
    std::unique_ptr<IExecStrategy> strategy_;
};

} // namespace exec
