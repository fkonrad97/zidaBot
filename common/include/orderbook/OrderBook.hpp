#pragma once

#include <vector>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

struct Level {
    std::int64_t priceTick;
    std::int64_t quantityLot;

    std::string price;
    std::string quantity;

    bool isEmpty() const { return (quantityLot == 0); }
};

enum class Side {
    BID,
    ASK
};

/**
 * Notes:
 * - To confirm empirically, compile with optimizations (-O2/-O3) and check the emitted assembly (e.g., Compiler Explorer).
 *   It should show two separate instantiations where the container address is fixed.
 * - 'lower_bound' does a binary search O(log(N) and returns the insertion point.
 * - Check which is faster in this case, 'lower_bound' or 'find_if'
 * - Both above needs already sorted lists !!!
 */
namespace md {
    class OrderBook {
    public:
        explicit OrderBook(std::size_t depth) : depth(depth) {
            if (depth == 0)
                throw std::invalid_argument("OrderBook: depth must be greater than 0");

            /// For both vector, (depth + 1) space alocated. The plus 1 is needed to not have any reallocation,
            /// when the update of the Order Book exceeds the depth by 1 if it full and we have an update to it.
            /// The update first insert and then pop_back,
            /// so for a small fraction of time, we exceeds depth, and that would result reallocation. (See Option C. at update())
            bids.reserve(depth + 1);
            asks.reserve(depth + 1);
        }

        const Level *bid_ptr(std::size_t i) const noexcept {
            if (i >= bids.size()) return nullptr;
            if (bids[i].isEmpty()) return nullptr;
            return &bids[i];
        }

        const Level *ask_ptr(std::size_t i) const noexcept {
            if (i >= asks.size()) return nullptr;
            if (asks[i].isEmpty()) return nullptr;
            return &asks[i];
        }


        /**
         * 'update' handes the incoming updates to the order book from the exchange
         * @tparam S - side of the orderbook (ask or bid)
         * @param level - the update coming from the exchange
         */
        template<Side S>
        void update(const Level &level) {
            if (level.isEmpty()) {
                this->remove<S>(level.priceTick);
                return;
            }

            std::vector<Level> &vec = (S == Side::BID) ? bids : asks;
            std::vector<Level>::iterator it = vec.begin();
            const std::int64_t updateTick = level.priceTick;

            if constexpr (S == Side::BID) {
                it = std::lower_bound(vec.begin(), vec.end(), updateTick,
                                      [](const Level &obLevel, std::int64_t _updateTick) {
                                          /// 'it' becomes the first position where ob.priceTick > tick is false
                                          return obLevel.priceTick > _updateTick;
                                      });
            } else {
                it = std::lower_bound(vec.begin(), vec.end(), updateTick,
                                      [](const Level &obLevel, std::int64_t _updateTick) {
                                          return obLevel.priceTick < _updateTick;
                                      });
            }

            /// Option A.: The item is in the order book, just the quantity of it updates.
            if (it != vec.end() && it->priceTick == updateTick) {
                it->quantityLot = level.quantityLot;
                return;
            }

            /// Option B.: If we have room, insert anywhere (including end)
            if (vec.size() < depth) {
                vec.insert(it, level);
                return;
            }

            /// Option C.: insert only if it improves top-N (i.e., not at end)
            if (it != vec.end()) {
                vec.insert(it, level);
                vec.pop_back(); // drops last element, since depth have to be ensured
            }
        }

        /**
         * 'remove' erase the level which got quantity=0 update
         * std::find_if does a linear scan - O(n)
         * std::lower_bound does a binary search - O(log(n))
         */
        template<Side S>
        void remove(std::int64_t priceTick) {
            std::vector<Level> &vec = S == Side::BID ? bids : asks; // resolved at compile time
            std::vector<Level>::iterator it = vec.begin();

            if constexpr (S == Side::BID) {
                it = std::lower_bound(vec.begin(), vec.end(), priceTick,
                                      [](const Level &obLevel, std::int64_t _priceTick) {
                                          return obLevel.priceTick > _priceTick;
                                      });
            } else {
                it = std::lower_bound(vec.begin(), vec.end(), priceTick,
                                      [](const Level &obLevel, std::int64_t _priceTick) {
                                          return obLevel.priceTick < _priceTick;
                                      });
            }

            if (it != vec.end() && it->priceTick == priceTick) {
                vec.erase(it);
            }
        }

        /**
 * Check price level uniqueness and sortedness
 */
        [[nodiscard]] bool validate() const {
            // 1) Depth sanity
            if (bids.size() > depth) return false;
            if (asks.size() > depth) return false;

            // 2) Sortedness + uniqueness:
            // bids must be strictly decreasing: bids[i-1].priceTick > bids[i].priceTick
            for (std::size_t i = 1; i < bids.size(); ++i) {
                if (bids[i - 1].priceTick <= bids[i].priceTick) {
                    return false; // not sorted DESC or duplicate tick
                }
            }

            // asks must be strictly increasing: asks[i-1].priceTick < asks[i].priceTick
            for (std::size_t i = 1; i < asks.size(); ++i) {
                if (asks[i - 1].priceTick >= asks[i].priceTick) {
                    return false; // not sorted ASC or duplicate tick
                }
            }

            // 3) Reject empty levels lingering in book if there can be any
            for (const Level &l: bids) if (l.isEmpty()) return false;
            for (const Level &l: asks) if (l.isEmpty()) return false;

            return true;
        }

        /**
         * Clears the entire book.
         * 
         * - All levels on both sides are reset to empty (Level{}).
         * - Depth and vector sizes are preserved: bids_.size() == asks_.size() == depth_.
         */
        void clear() noexcept {
            bids.clear();
            asks.clear();
        }

        /**
         * Returns the top-of-book bid level (index 0).
         */
        [[nodiscard]] const Level &best_bid() const noexcept {
            static const Level empty{0, 0, "", ""};
            if (bids.empty()) return empty;
            return bids.front();
        }

        /**
         * Returns the top-of-book ask level (index 0).
         */
        [[nodiscard]] const Level &best_ask() const noexcept {
            static const Level empty{0, 0, "", ""};
            if (asks.empty()) return empty;
            return asks.front();
        }

    private:
        std::size_t depth;

        /**
         * sorted descending by priceTick
         */
        std::vector<Level> bids;

        /**
         * sorted ascending by priceTick
         */
        std::vector<Level> asks;
    };
}
