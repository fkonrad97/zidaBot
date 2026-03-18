#include "orderbook/OrderBookController.hpp"

#include <algorithm>
#include <iostream>

namespace md {
    const char *OrderBookController::book_sync_state_to_string_(BookSyncState state) noexcept {
        switch (state) {
            case BookSyncState::WaitingSnapshot: return "WaitingSnapshot";
            case BookSyncState::WaitingBridge: return "WaitingBridge";
            case BookSyncState::Synced: return "Synced";
            default: return "Unknown";
        }
    }

    void OrderBookController::set_state_(BookSyncState next, std::string_view reason) noexcept {
        if (state_ == next) return;
        std::cerr << "[OrderBookController] state "
                  << book_sync_state_to_string_(state_)
                  << " -> " << book_sync_state_to_string_(next);
        if (!reason.empty()) {
            std::cerr << " reason=" << reason;
        }
        std::cerr << "\n";
        state_ = next;
    }

    OrderBookController::Action OrderBookController::need_resync_(std::string_view reason,
                                                                  std::uint64_t first_seq,
                                                                  std::uint64_t last_seq,
                                                                  std::uint64_t expected_seq) {
        std::cerr << "[OrderBookController] need resync"
                  << " state=" << book_sync_state_to_string_(state_);
        if (!reason.empty()) {
            std::cerr << " reason=" << reason;
        }
        if (first_seq != 0 || last_seq != 0 || expected_seq != 0) {
            std::cerr << " first_seq=" << first_seq
                      << " last_seq=" << last_seq
                      << " expected_seq=" << expected_seq;
        }
        std::cerr << "\n";
        resetBook();
        return Action::NeedResync;
    }

    OrderBookController::Action
    OrderBookController::onSnapshot(const GenericSnapshotFormat &msg, BaselineKind kind) {
        resetBook();
        std::cerr << "[OrderBookController] applying snapshot"
                  << " baseline=" << (kind == BaselineKind::WsAuthoritative ? "ws_authoritative" : "rest_anchored")
                  << " seq=" << msg.lastUpdateId
                  << " bids=" << msg.bids.size()
                  << " asks=" << msg.asks.size()
                  << "\n";

        std::vector<Level> bids = msg.bids;
        std::vector<Level> asks = msg.asks;

        std::sort(asks.begin(), asks.end(),
                  [](const Level &x, const Level &y) { return x.priceTick < y.priceTick; });

        std::sort(bids.begin(), bids.end(),
                  [](const Level &x, const Level &y) { return x.priceTick > y.priceTick; });

        for (const Level &lvl: bids) book_.update<Side::BID>(lvl);
        for (const Level &lvl: asks) book_.update<Side::ASK>(lvl);

        last_seq_ = msg.lastUpdateId;
        expected_seq_ = last_seq_ + 1;

        const bool checksum_enabled = (checksum_fn_ != nullptr);

        // If checksum is enabled and a checksum was actually provided, validate it.
        // A snapshot with checksum==0 is accepted as best-effort (some venues don't
        // include a checksum on the initial baseline but do include them on incrementals).
        if (checksum_enabled) {
            if (msg.checksum == 0) {
                std::cerr << "[OrderBookController] snapshot has no checksum (best-effort baseline)\n";
                // Continue without validation — first incremental checksum will verify.
            } else if (!validateChecksum(msg.checksum)) {
                return need_resync_("snapshot_checksum_mismatch");
            }
        }

        set_state_((kind == BaselineKind::WsAuthoritative)
                       ? BookSyncState::Synced
                       : BookSyncState::WaitingBridge,
                   "snapshot_applied");

        return Action::None;
    }

    OrderBookController::Action
    OrderBookController::onIncrement(const GenericIncrementalFormat &msg) {
        if (state_ == BookSyncState::WaitingSnapshot) {
            return Action::None; // handler buffers
        }

        const bool has_seq = (msg.last_seq != 0);
        const bool checksum_enabled = (checksum_fn_ != nullptr);

        if (!has_seq && !checksum_enabled) {
            return need_resync_("incremental_missing_seq_and_checksum");
        }

        // ---- Bridging phase (RestAnchored) ----
        if (state_ == BookSyncState::WaitingBridge) {
            if (has_seq) {
                const std::uint64_t required = expected_seq_;

                std::cerr << "[CTRL][BRIDGE] last_seq_=" << last_seq_
                        << " required=" << required
                        << " msg.first=" << msg.first_seq
                        << " msg.last=" << msg.last_seq
                        << "\n";

                if (msg.last_seq < required) {
                    std::cerr << "[CTRL][BRIDGE] IGNORE too-old: msg.last < required ("
                            << msg.last_seq << " < " << required << ")\n";
                    return Action::None;
                }

                if (msg.first_seq > required) {
                    if (!allow_seq_gap_) {
                        std::cerr << "[CTRL][BRIDGE] RESYNC gap: msg.first > required ("
                                << msg.first_seq << " > " << required << ")\n";
                        return need_resync_("bridge_sequence_gap", msg.first_seq, msg.last_seq, required);
                    } else {
                        std::cerr << "[CTRL][BRIDGE] GAP tolerated (allow_seq_gap_)\n";
                        // fall through and apply below
                    }
                }

                // Covers required (overlap OK for absolute level-set updates) or
                // gap-tolerated case.
                std::cerr << "[CTRL][BRIDGE] APPLY covers required=" << required
                        << " (first=" << msg.first_seq << ", last=" << msg.last_seq << ")\n";

                applyIncrementUpdate(msg);
                last_seq_ = msg.last_seq;
                expected_seq_ = last_seq_ + 1;
                set_state_(BookSyncState::Synced, "bridge_incremental_applied");

                std::cerr << "[CTRL][BRIDGE] -> Synced last_seq_=" << last_seq_
                        << " expected_seq_=" << expected_seq_ << "\n";
            } else {
                std::cerr << "[CTRL][BRIDGE] APPLY seq-less -> Synced\n";
                applyIncrementUpdate(msg);
                set_state_(BookSyncState::Synced, "bridge_seqless_incremental_applied");
            }

            // C1: crossed-book guard
            {
                const auto &bid = book_.best_bid();
                const auto &ask = book_.best_ask();
                if (bid.priceTick > 0 && ask.priceTick > 0 && bid.priceTick >= ask.priceTick) {
                    std::cerr << "[OrderBookController] book_crossed after bridge"
                              << " bid=" << bid.priceTick << " ask=" << ask.priceTick << "\n";
                    return need_resync_("book_crossed_bridge", msg.first_seq, msg.last_seq, expected_seq_);
                }
            }

            if (checksum_enabled) {
                if (msg.checksum == 0) {
                    std::cerr << "[CTRL][BRIDGE] RESYNC missing checksum while enabled\n";
                    return need_resync_("bridge_missing_checksum", msg.first_seq, msg.last_seq, expected_seq_);
                }
                if (!validateChecksum(msg.checksum)) {
                    std::cerr << "[CTRL][BRIDGE] RESYNC checksum mismatch\n";
                    return need_resync_("bridge_checksum_mismatch", msg.first_seq, msg.last_seq, expected_seq_);
                }
            }

            return Action::None;
        }

        // ---- Steady-state (Synced) ----
        if (has_seq) {
            const std::uint64_t required = expected_seq_;
            std::cerr << "[OrderBookController] applying steady_state incremental"
                      << " required_seq=" << required
                      << " first_seq=" << msg.first_seq
                      << " last_seq=" << msg.last_seq
                      << " bids=" << msg.bids.size()
                      << " asks=" << msg.asks.size()
                      << "\n";

            if (msg.last_seq < required) return Action::None; // outdated
            if (msg.first_seq > required) {
                if (!allow_seq_gap_) {
                    return need_resync_("steady_state_sequence_gap", msg.first_seq, msg.last_seq, required);
                }
                // allow_seq_gap_ == true -> tolerate gap and continue
            }

            // overlap/cover is OK (or gap tolerated)
            applyIncrementUpdate(msg);
            last_seq_ = msg.last_seq;
            expected_seq_ = last_seq_ + 1;
        } else {
            // seq-less venue: checksum is the integrity guard
            applyIncrementUpdate(msg);
        }

        // C1: crossed-book guard — bid >= ask means data corruption; resync immediately
        {
            const auto &bid = book_.best_bid();
            const auto &ask = book_.best_ask();
            if (bid.priceTick > 0 && ask.priceTick > 0 && bid.priceTick >= ask.priceTick) {
                std::cerr << "[OrderBookController] book_crossed after incremental"
                          << " bid=" << bid.priceTick << " ask=" << ask.priceTick << "\n";
                return need_resync_("book_crossed", msg.first_seq, msg.last_seq, expected_seq_);
            }
        }

        // C3: periodic sort-order / uniqueness validation
        if (validate_period_ > 0) {
            if (++validate_counter_ >= validate_period_) {
                validate_counter_ = 0;
                if (!book_.validate()) {
                    std::cerr << "[OrderBookController] validate() failed after incremental\n";
                    return need_resync_("validate_failed", msg.first_seq, msg.last_seq, expected_seq_);
                }
            }
        }

        if (checksum_enabled) {
            if (msg.checksum == 0) {
                return need_resync_("steady_state_missing_checksum", msg.first_seq, msg.last_seq, expected_seq_);
            }
            if (!validateChecksum(msg.checksum)) {
                return need_resync_("steady_state_checksum_mismatch", msg.first_seq, msg.last_seq, expected_seq_);
            }
        }

        return Action::None;
    }

    void OrderBookController::applyIncrementUpdate(const GenericIncrementalFormat &upd) {
        for (const Level &lvl: upd.bids) book_.update<Side::BID>(lvl);
        for (const Level &lvl: upd.asks) book_.update<Side::ASK>(lvl);
    }
} // namespace md
