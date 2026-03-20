#include "orderbook/OrderBookController.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>

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
        if (reason.empty()) {
            spdlog::info("[OBC] state {} -> {}",
                         book_sync_state_to_string_(state_), book_sync_state_to_string_(next));
        } else {
            spdlog::info("[OBC] state {} -> {} reason={}",
                         book_sync_state_to_string_(state_), book_sync_state_to_string_(next), reason);
        }
        state_ = next;
    }

    OrderBookController::Action OrderBookController::need_resync_(std::string_view reason,
                                                                  std::uint64_t first_seq,
                                                                  std::uint64_t last_seq,
                                                                  std::uint64_t expected_seq) {
        if (first_seq != 0 || last_seq != 0 || expected_seq != 0) {
            spdlog::warn("[OBC] need_resync state={} reason={} first_seq={} last_seq={} expected_seq={}",
                         book_sync_state_to_string_(state_), reason, first_seq, last_seq, expected_seq);
        } else {
            spdlog::warn("[OBC] need_resync state={} reason={}",
                         book_sync_state_to_string_(state_), reason);
        }
        resetBook();
        return Action::NeedResync;
    }

    OrderBookController::Action
    OrderBookController::onSnapshot(const GenericSnapshotFormat &msg, BaselineKind kind) {
        resetBook();
        spdlog::info("[OBC] applying snapshot baseline={} seq={} bids={} asks={}",
                     (kind == BaselineKind::WsAuthoritative ? "ws_authoritative" : "rest_anchored"),
                     msg.lastUpdateId, msg.bids.size(), msg.asks.size());

        std::vector<Level> bids = msg.bids;
        std::vector<Level> asks = msg.asks;

        // B7: zero-qty levels in a snapshot are anomalous (in incrementals they are
        // valid remove signals, but a snapshot should only contain resting liquidity).
        auto skip_zero_qty = [](const Level &l) {
            if (l.quantityLot == 0) {
                spdlog::debug("[OBC] snapshot zero-qty level skipped priceTick={}", l.priceTick);
                return true;
            }
            return false;
        };
        std::erase_if(bids, skip_zero_qty);
        std::erase_if(asks, skip_zero_qty);

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
                spdlog::debug("[OBC] snapshot has no checksum (best-effort baseline)");
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

                spdlog::debug("[OBC][BRIDGE] last_seq_={} required={} msg.first={} msg.last={}",
                              last_seq_, required, msg.first_seq, msg.last_seq);

                if (msg.last_seq < required) {
                    spdlog::debug("[OBC][BRIDGE] IGNORE too-old: msg.last < required ({} < {})",
                                  msg.last_seq, required);
                    return Action::None;
                }

                if (msg.first_seq > required) {
                    if (!allow_seq_gap_) {
                        spdlog::debug("[OBC][BRIDGE] RESYNC gap: msg.first > required ({} > {})",
                                      msg.first_seq, required);
                        return need_resync_("bridge_sequence_gap", msg.first_seq, msg.last_seq, required);
                    } else {
                        spdlog::debug("[OBC][BRIDGE] GAP tolerated (allow_seq_gap_)");
                        // fall through and apply below
                    }
                }

                // Covers required (overlap OK for absolute level-set updates) or
                // gap-tolerated case.
                spdlog::debug("[OBC][BRIDGE] APPLY covers required={} (first={}, last={})",
                              required, msg.first_seq, msg.last_seq);

                applyIncrementUpdate(msg);
                last_seq_ = msg.last_seq;
                expected_seq_ = last_seq_ + 1;
                set_state_(BookSyncState::Synced, "bridge_incremental_applied");

                spdlog::debug("[OBC][BRIDGE] -> Synced last_seq_={} expected_seq_={}",
                              last_seq_, expected_seq_);
            } else {
                spdlog::debug("[OBC][BRIDGE] APPLY seq-less -> Synced");
                applyIncrementUpdate(msg);
                set_state_(BookSyncState::Synced, "bridge_seqless_incremental_applied");
            }

            // C1: crossed-book guard
            {
                const auto &bid = book_.best_bid();
                const auto &ask = book_.best_ask();
                if (bid.priceTick > 0 && ask.priceTick > 0 && bid.priceTick >= ask.priceTick) {
                    spdlog::warn("[OBC] book_crossed after bridge bid={} ask={}",
                                 bid.priceTick, ask.priceTick);
                    return need_resync_("book_crossed_bridge", msg.first_seq, msg.last_seq, expected_seq_);
                }
            }

            if (checksum_enabled) {
                if (msg.checksum == 0) {
                    spdlog::debug("[OBC][BRIDGE] RESYNC missing checksum while enabled");
                    return need_resync_("bridge_missing_checksum", msg.first_seq, msg.last_seq, expected_seq_);
                }
                if (!validateChecksum(msg.checksum)) {
                    spdlog::debug("[OBC][BRIDGE] RESYNC checksum mismatch");
                    return need_resync_("bridge_checksum_mismatch", msg.first_seq, msg.last_seq, expected_seq_);
                }
            }

            return Action::None;
        }

        // ---- Steady-state (Synced) ----
        if (has_seq) {
            const std::uint64_t required = expected_seq_;
            spdlog::debug("[OBC] steady_state incremental required_seq={} first_seq={} last_seq={} bids={} asks={}",
                          required, msg.first_seq, msg.last_seq, msg.bids.size(), msg.asks.size());

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
                spdlog::warn("[OBC] book_crossed after incremental bid={} ask={}",
                             bid.priceTick, ask.priceTick);
                return need_resync_("book_crossed", msg.first_seq, msg.last_seq, expected_seq_);
            }
        }

        // C3: periodic sort-order / uniqueness validation
        if (validate_period_ > 0) {
            if (++validate_counter_ >= validate_period_) {
                validate_counter_ = 0;
                if (!book_.validate()) {
                    spdlog::warn("[OBC] validate() failed after incremental");
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

        // C5: --require-checksum strict mode: venue declares checksums but field absent
        // and no validation fn is wired (e.g. future re-enabled venue without fn yet).
        if (require_checksum_ && has_checksum_ && !checksum_fn_ && msg.checksum == 0) {
            return need_resync_("required_checksum_absent", msg.first_seq, msg.last_seq, expected_seq_);
        }

        return Action::None;
    }

    void OrderBookController::applyIncrementUpdate(const GenericIncrementalFormat &upd) {
        for (const Level &lvl: upd.bids) book_.update<Side::BID>(lvl);
        for (const Level &lvl: upd.asks) book_.update<Side::ASK>(lvl);
    }
} // namespace md
