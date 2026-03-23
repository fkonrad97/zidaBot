#include "postprocess/FilePersistSink.hpp"

#include <chrono>
#include <filesystem>

namespace md {
    namespace {
        bool has_gzip_suffix(const std::string &path) {
            constexpr std::string_view suffix = ".gz";
            return path.size() >= suffix.size()
                   && std::string_view(path).substr(path.size() - suffix.size()) == suffix;
        }
    }

    FilePersistSink::FilePersistSink(std::string path, std::string venue, std::string symbol,
                                     std::uint64_t max_file_bytes)
        : path_(std::move(path)),
          venue_(std::move(venue)),
          symbol_(std::move(symbol)),
          max_file_bytes_(max_file_bytes) {
        try {
            const std::filesystem::path p(path_);
            if (p.has_parent_path()) {
                std::filesystem::create_directories(p.parent_path());
            }
            if (use_gzip_()) {
                gz_out_ = gzopen(path_.c_str(), "ab");
                file_open_.store(gz_out_ != nullptr, std::memory_order_relaxed);
            } else {
                out_.open(path_, std::ios::out | std::ios::app);
                file_open_.store(out_.is_open(), std::memory_order_relaxed);
            }
        } catch (...) {
            // Keep sink disabled if path setup/open fails.
        }
        // G3: start writer thread (it will do nothing if file_open_ is false)
        writer_running_.store(true, std::memory_order_relaxed);
        writer_thread_ = std::thread(&FilePersistSink::writer_loop_, this);
    }

    FilePersistSink::~FilePersistSink() {
        // G3: signal writer thread to drain and exit; file handles closed inside writer_loop_.
        writer_running_.store(false, std::memory_order_relaxed);
        write_cv_.notify_all();
        if (writer_thread_.joinable()) writer_thread_.join();
    }

    std::int64_t FilePersistSink::now_ns_() noexcept {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    }

    bool FilePersistSink::use_gzip_() const noexcept {
        return has_gzip_suffix(path_);
    }

    nlohmann::json FilePersistSink::levels_to_json_(const std::vector<Level> &levels) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &lvl: levels) {
            arr.push_back({
                {"price", lvl.price},
                {"quantity", lvl.quantity},
                {"priceTick", lvl.priceTick},
                {"quantityLot", lvl.quantityLot}
            });
        }
        return arr;
    }

    nlohmann::json FilePersistSink::levels_from_book_(const OrderBook &book, std::size_t top_n, Side side) {
        nlohmann::json arr = nlohmann::json::array();
        for (std::size_t i = 0; i < top_n; ++i) {
            const Level *lvl = (side == Side::BID) ? book.bid_ptr(i) : book.ask_ptr(i);
            if (!lvl) break;
            arr.push_back({
                {"price", lvl->price},
                {"quantity", lvl->quantity},
                {"priceTick", lvl->priceTick},
                {"quantityLot", lvl->quantityLot}
            });
        }
        return arr;
    }

    void FilePersistSink::rotate_() noexcept {
        // Only rotate plain JSONL; gzip rotation is not yet supported.
        // Called from writer_loop_ (writer thread only).
        if (gz_out_ != nullptr || !out_.is_open()) return;
        out_.flush();
        out_.close();

        ++rotate_seq_;
        const std::string rotated = path_ + "." + std::to_string(rotate_seq_);
        std::rename(path_.c_str(), rotated.c_str());

        out_.open(path_, std::ios::out | std::ios::trunc);
        bytes_written_ = 0;
    }

    void FilePersistSink::enqueue_line_(std::string line) noexcept {
        // G3: called on handler I/O thread — non-blocking.
        std::lock_guard lk(write_mu_);
        while (write_queue_.size() >= kWriteQueueCap)
            write_queue_.pop();          // drop oldest — prefer freshness
        write_queue_.push(std::move(line));
        write_cv_.notify_one();
    }

    void FilePersistSink::writer_loop_() noexcept {
        // G3: background writer — owns all disk I/O.
        while (true) {
            std::string line;
            {
                std::unique_lock lk(write_mu_);
                write_cv_.wait(lk, [&] {
                    return !write_queue_.empty()
                        || !writer_running_.load(std::memory_order_relaxed);
                });
                if (write_queue_.empty()) break;   // shutdown: queue drained
                line = std::move(write_queue_.front());
                write_queue_.pop();
            }
            try {
                if (gz_out_ != nullptr) {
                    const int rc = gzwrite(gz_out_, line.data(),
                                           static_cast<unsigned int>(line.size()));
                    if (rc > 0) gzflush(gz_out_, Z_SYNC_FLUSH);
                } else if (out_.is_open()) {
                    out_ << line;
                    out_.flush();
                    bytes_written_ += static_cast<std::uint64_t>(line.size());
                    if (max_file_bytes_ > 0 && bytes_written_ >= max_file_bytes_)
                        rotate_();
                }
            } catch (...) {}
        }
        // Close file handles on exit — destructor has already joined us.
        if (gz_out_ != nullptr) { gzclose(gz_out_); gz_out_ = nullptr; }
        if (out_.is_open())     { out_.flush(); out_.close(); }
        file_open_.store(false, std::memory_order_relaxed);
    }

    void FilePersistSink::write_snapshot(const GenericSnapshotFormat &snap, std::string_view source) noexcept {
        nlohmann::json j;
        const auto ts_persist_ns = now_ns_();
        j["schema_version"] = 1;
        j["event_type"] = "snapshot";
        j["source"] = source;
        j["venue"] = venue_;
        j["symbol"] = symbol_;
        j["persist_seq"] = ++persist_seq_;
        j["ts_recv_ns"] = snap.ts_recv_ns;
        j["ts_persist_ns"] = ts_persist_ns;
        j["seq_first"] = snap.lastUpdateId;
        j["seq_last"] = snap.lastUpdateId;
        j["checksum"] = snap.checksum;
        j["bids"] = levels_to_json_(snap.bids);
        j["asks"] = levels_to_json_(snap.asks);
        enqueue_line_(j.dump() + '\n');
    }

    void FilePersistSink::write_incremental(const GenericIncrementalFormat &inc, std::string_view source) noexcept {
        nlohmann::json j;
        const auto ts_persist_ns = now_ns_();
        j["schema_version"] = 1;
        j["event_type"] = "incremental";
        j["source"] = source;
        j["venue"] = venue_;
        j["symbol"] = symbol_;
        j["persist_seq"] = ++persist_seq_;
        j["ts_recv_ns"] = inc.ts_recv_ns;
        j["ts_persist_ns"] = ts_persist_ns;
        j["seq_first"] = inc.first_seq;
        j["seq_last"] = inc.last_seq;
        j["prev_last"] = inc.prev_last;
        j["checksum"] = inc.checksum;
        j["bids"] = levels_to_json_(inc.bids);
        j["asks"] = levels_to_json_(inc.asks);
        enqueue_line_(j.dump() + '\n');
    }

    void FilePersistSink::write_book_state(const OrderBook &book,
                                           std::uint64_t applied_seq,
                                           std::size_t top_n,
                                           std::string_view source,
                                           std::int64_t ts_book_ns) noexcept {
        nlohmann::json j;
        const auto ts_persist_ns = now_ns_();
        j["schema_version"] = 1;
        j["event_type"] = "book_state";
        j["source"] = source;
        j["venue"] = venue_;
        j["symbol"] = symbol_;
        j["persist_seq"] = ++persist_seq_;
        j["ts_recv_ns"] = 0;
        j["ts_book_ns"] = ts_book_ns;
        j["ts_persist_ns"] = ts_persist_ns;
        j["applied_seq"] = applied_seq;
        j["top_n"] = top_n;
        j["bids"] = levels_from_book_(book, top_n, Side::BID);
        j["asks"] = levels_from_book_(book, top_n, Side::ASK);
        enqueue_line_(j.dump() + '\n');
    }
}
