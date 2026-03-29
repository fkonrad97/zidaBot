#pragma once

#include "brain/ArbDetector.hpp"

namespace exec {

/// Pluggable execution strategy interface.
/// ExecEngine calls on_signal() after all E1–E4 guards pass.
/// The strategy is fully responsible for order sizing, submission
/// via IOrderClient, and registering pending orders with OrderTracker.
/// All work must be non-blocking — defer async work via the Asio strand.
class IExecStrategy {
public:
    virtual ~IExecStrategy() = default;

    /// Called once per ArbCross that passes all ExecEngine guards.
    /// Must return immediately — no blocking I/O on the calling strand.
    virtual void on_signal(const brain::ArbCross &cross) = 0;

    /// E2 kill switch — strategy must stop submitting new orders when paused.
    virtual void pause()  {}
    virtual void resume() {}
};

} // namespace exec