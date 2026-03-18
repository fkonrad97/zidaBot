#pragma once

#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include "orderbook/OrderBookController.hpp"

namespace md {
    class FilePersistSink {
    public:
        /// @param max_file_bytes  D3: rotate output after this many bytes (0 = no rotation).
        ///                        Only applies to plain JSONL output; .gz rotation is not
        ///                        yet supported (requires gzclose + gzopen).
        FilePersistSink(std::string path, std::string venue, std::string symbol,
                        std::uint64_t max_file_bytes = 0);
        ~FilePersistSink();

        [[nodiscard]] bool is_open() const noexcept { return out_.is_open() || gz_out_ != nullptr; }

        void write_snapshot(const GenericSnapshotFormat &snap, std::string_view source) noexcept;
        void write_incremental(const GenericIncrementalFormat &inc, std::string_view source) noexcept;
        void write_book_state(const OrderBook &book,
                              std::uint64_t applied_seq,
                              std::size_t top_n,
                              std::string_view source,
                              std::int64_t ts_book_ns) noexcept;

    private:
        static std::int64_t now_ns_() noexcept;
        static nlohmann::json levels_to_json_(const std::vector<Level> &levels);
        static nlohmann::json levels_from_book_(const OrderBook &book, std::size_t top_n, Side side);
        void write_line_(const nlohmann::json &j) noexcept;
        [[nodiscard]] bool use_gzip_() const noexcept;
        /// D3: rotate plain JSONL output when size limit is reached.
        void rotate_() noexcept;

    private:
        std::ofstream out_;
        gzFile gz_out_{nullptr};
        std::string path_;
        std::string venue_;
        std::string symbol_;
        std::uint64_t persist_seq_{0};
        std::uint64_t max_file_bytes_{0};  ///< D3: 0 = no rotation
        std::uint64_t bytes_written_{0};   ///< D3: bytes written to current file
        std::uint32_t rotate_seq_{0};      ///< D3: rotation counter
    };
}
