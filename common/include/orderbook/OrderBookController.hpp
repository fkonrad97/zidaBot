#pragma once

#include <vector>
#include <deque>
#include "OrderBook.hpp"
#include "utils/CheckSumUtils.hpp"

struct GenericIncrementalFormat {
    std::uint64_t first_seq{0};
    std::uint64_t last_seq{0};
    std::uint64_t prev_last{0};
    std::int64_t ts_recv_ns{0}; // local receive timestamp at ingestion point

    std::int64_t checksum{0};

    std::vector<Level> bids;
    std::vector<Level> asks;

    void reset() noexcept {
        first_seq = last_seq = prev_last = 0;
        ts_recv_ns = 0;
        checksum = 0;
        bids.clear();
        asks.clear();
    }
};

struct GenericSnapshotFormat {
    std::uint64_t lastUpdateId{0};
    std::int64_t ts_recv_ns{0}; // local receive timestamp at ingestion point

    std::int64_t checksum{0};

    std::vector<Level> bids;
    std::vector<Level> asks;

    void reset() noexcept {
        lastUpdateId = 0;
        ts_recv_ns = 0;
        checksum = 0;
        bids.clear();
        asks.clear();
    }
};

namespace md {
    class OrderBookController {
    public:
        explicit OrderBookController(const std::size_t depth) : book_{depth} {
        }

        ~OrderBookController() = default;

        void configureChecksum(ChecksumFn fn, std::size_t topN) noexcept {
            checksum_fn_ = fn;
            checksum_topN_ = topN;
        }

        /// In some venues (e.g. KuCoin) sequence numbers may jump when snapshot is
        /// partial; enabling this flag instructs the controller to tolerate gaps
        /// instead of forcing a resync.  Defaults to false (strict continuity).
        void setAllowSequenceGap(bool allow) noexcept { allow_seq_gap_ = allow; }

        /// C3: call OrderBook::validate() every N applied incremental updates.
        /// On a sort-order or uniqueness violation, triggers a resync.  0 = disabled.
        void setValidatePeriod(std::size_t n) noexcept { validate_period_ = n; }

        enum class BaselineKind : std::uint8_t { RestAnchored, WsAuthoritative };

        enum class Action {
            None,
            NeedResync
        };

        enum class BookSyncState : std::uint8_t {
            WaitingSnapshot,
            WaitingBridge, // have snapshot, waiting for bridging incremental (RestAnchored)
            Synced
        };

        /**
         * 'onSnapshot' process the incoming snapshot message from the exchange
         * @param msg - snapshot message
         */
        Action onSnapshot(const GenericSnapshotFormat &msg, BaselineKind kind);

        /**
         * 'onIncrement' process the incoming incremental update message from the exchange
         * @param msg - incremental update message
         */
        Action onIncrement(const GenericIncrementalFormat &msg);

        void resetBook() {
            book_.clear();
            state_ = BookSyncState::WaitingSnapshot;
            last_seq_ = 0;
            expected_seq_ = 0;
        }

        [[nodiscard]] const OrderBook &book() const noexcept { return book_; }

        /**
         * 'isSynced' indicates whether the order book is currently synchronized with the exchange data feed.
         */
        [[nodiscard]] bool isSynced() const noexcept {
            return state_ == BookSyncState::Synced;
        }

        /**
         * 'getSyncState' retrieves the current synchronization state of the order book.
         */
        BookSyncState getSyncState() const noexcept { return state_; }

        /**
         * 'getAppliedSeqID' retrieves the last sequence ID that has been successfully applied to the order book.
         * 'setAppliedSeqID' sets the last sequence ID that has been successfully applied to the order book.
         */
        [[nodiscard]] std::uint64_t getAppliedSeqID() const noexcept { return last_seq_; }

    private:
        static const char *book_sync_state_to_string_(BookSyncState state) noexcept;
        void set_state_(BookSyncState next, std::string_view reason) noexcept;
        Action need_resync_(std::string_view reason,
                            std::uint64_t first_seq = 0,
                            std::uint64_t last_seq = 0,
                            std::uint64_t expected_seq = 0);

        OrderBook book_;
        BookSyncState state_{BookSyncState::WaitingSnapshot};

        std::uint64_t last_seq_{0};
        std::uint64_t expected_seq_{0}; // next expected first_seq for continuous stream

        ChecksumFn checksum_fn_{nullptr};
        std::size_t checksum_topN_{25};

        bool allow_seq_gap_{false};

        // C3: periodic book validation
        std::size_t validate_period_{0};   ///< 0 = disabled; else validate every N updates
        std::size_t validate_counter_{0};

        bool validateChecksum(std::int64_t expected) const noexcept {
            if (!checksum_fn_) return true;
            return checksum_fn_(book_, expected, checksum_topN_);
        }

        void applyIncrementUpdate(const GenericIncrementalFormat &upd);
    };
}
