#include "postprocess/WsPublishSink.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace md {
    namespace {
        constexpr int kReconnectDelayMinMs = 1000;
        constexpr int kReconnectDelayMaxMs = 30'000;
        // Outbox cap: how many messages to queue while the socket is not writable.
        // Oldest messages are dropped when the cap is exceeded (prefer freshest updates).
        constexpr std::size_t kDefaultMaxOutbox = 5000;
    }

    WsPublishSink::WsPublishSink(boost::asio::io_context &ioc,
                                 std::string host,
                                 std::string port,
                                 std::string target,
                                 bool insecure_tls,
                                 std::string venue,
                                 std::string symbol)
        : ioc_(ioc),
          ws_(WsClient::create(ioc)),
          reconnect_timer_(ioc),
          host_(std::move(host)),
          port_(std::move(port)),
          target_(std::move(target)),
          venue_(std::move(venue)),
          symbol_(std::move(symbol)),
          insecure_tls_(insecure_tls) {
        ws_->set_tls_verify_peer(!insecure_tls);
        // If brain is down or slow, keep memory bounded; prefer freshest updates.
        ws_->set_max_outbox(kDefaultMaxOutbox);
        ws_->set_on_raw_message([](const char *, std::size_t) {
            // Intentionally ignored; brain -> pop messages are not used yet.
        });
    }

    void WsPublishSink::start() {
        if (running_) return;
        running_ = true;
        reconnect_delay_ms_ = kReconnectDelayMinMs;
        reconnect_scheduled_ = false;
        reconnect_gen_++;

        ws_->set_on_open([this] {
            reconnect_delay_ms_ = kReconnectDelayMinMs;
            reconnect_scheduled_ = false;
            std::cerr << "[WsPublishSink] connected brain ws host=" << host_ << " port=" << port_ << " target=" << target_ << "\n";
            if (insecure_tls_)
                std::cerr << "[WsPublishSink] WARNING: TLS certificate verification is DISABLED (--brain_ws_insecure)\n";
        });

        ws_->set_on_close([this] {
            if (!running_) return;
            schedule_reconnect_();
        });

        ws_->set_logger([](std::string_view msg) {
            std::cerr << "[WsPublishSink] " << msg << "\n";
        });

        connect_();
    }

    void WsPublishSink::stop() {
        running_ = false;
        reconnect_scheduled_ = false;
        reconnect_gen_++;
        boost::system::error_code ignored;
        reconnect_timer_.cancel(ignored);
        if (ws_) ws_->close();
    }

    std::int64_t WsPublishSink::now_ns_() noexcept {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    }

    nlohmann::json WsPublishSink::levels_to_json_(const std::vector<Level> &levels) {
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

    nlohmann::json WsPublishSink::levels_from_book_(const OrderBook &book, std::size_t top_n, Side side) {
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

    void WsPublishSink::connect_() {
        if (!running_) return;
        try {
            ws_->connect(host_, port_, target_);
        } catch (...) {
            schedule_reconnect_();
        }
    }

    void WsPublishSink::schedule_reconnect_() {
        if (!running_) return;
        if (reconnect_scheduled_) return;
        reconnect_scheduled_ = true;

        const auto gen = ++reconnect_gen_;
        const int delay = reconnect_delay_ms_;
        reconnect_delay_ms_ = std::min(reconnect_delay_ms_ * 2, kReconnectDelayMaxMs);

        reconnect_timer_.expires_after(std::chrono::milliseconds(delay));
        reconnect_timer_.async_wait([this, gen](const boost::system::error_code &ec) {
            if (ec) return;
            if (!running_) return;
            if (gen != reconnect_gen_) return;
            reconnect_scheduled_ = false;
            connect_();
        });
    }

    void WsPublishSink::send_json_(const nlohmann::json &j) noexcept {
        try {
            // ts_persist_ns in the payload marks the time this message was enqueued,
            // not the time it was actually written to the socket.  Under a backlogged
            // outbox the two can diverge by several seconds.
            const std::string payload = j.dump();
            ws_->send_text(payload);
        } catch (...) {
            // Best-effort; keep feed alive even if publishing fails.
        }
    }

    void WsPublishSink::publish_status(std::string_view feed_state, std::string_view reason) noexcept {
        if (!running_) return;
        try {
            nlohmann::json j;
            j["schema_version"] = 1;
            j["event_type"]     = "status";
            j["venue"]          = venue_;
            j["symbol"]         = symbol_;
            j["feed_state"]     = feed_state;
            j["reason"]         = reason;
            j["ts_ns"]          = now_ns_();
            send_json_(j);
        } catch (...) {}
    }

    void WsPublishSink::publish_snapshot(const GenericSnapshotFormat &snap, std::string_view source) noexcept {
        if (!running_) return;
        nlohmann::json j;
        j["schema_version"] = 1;
        j["event_type"] = "snapshot";
        j["source"] = source;
        j["venue"] = venue_;
        j["symbol"] = symbol_;
        j["persist_seq"] = ++persist_seq_;
        j["ts_recv_ns"] = snap.ts_recv_ns;
        j["ts_persist_ns"] = now_ns_();
        // GenericSnapshotFormat carries a single sequence id (lastUpdateId).
        // seq_first == seq_last here; venues that distinguish them (e.g. Binance)
        // expose only seq_last through the normalized struct.
        j["seq_first"] = snap.lastUpdateId;
        j["seq_last"] = snap.lastUpdateId;
        j["checksum"] = snap.checksum;
        j["bids"] = levels_to_json_(snap.bids);
        j["asks"] = levels_to_json_(snap.asks);
        send_json_(j);
    }

    void WsPublishSink::publish_incremental(const GenericIncrementalFormat &inc, std::string_view source) noexcept {
        if (!running_) return;
        nlohmann::json j;
        j["schema_version"] = 1;
        j["event_type"] = "incremental";
        j["source"] = source;
        j["venue"] = venue_;
        j["symbol"] = symbol_;
        j["persist_seq"] = ++persist_seq_;
        j["ts_recv_ns"] = inc.ts_recv_ns;
        j["ts_persist_ns"] = now_ns_();
        j["seq_first"] = inc.first_seq;
        j["seq_last"] = inc.last_seq;
        j["prev_last"] = inc.prev_last;
        j["checksum"] = inc.checksum;
        j["bids"] = levels_to_json_(inc.bids);
        j["asks"] = levels_to_json_(inc.asks);
        send_json_(j);
    }

    void WsPublishSink::publish_book_state(const OrderBook &book,
                                           std::uint64_t applied_seq,
                                           std::size_t top_n,
                                           std::string_view source,
                                           std::int64_t ts_book_ns) noexcept {
        if (!running_) return;
        nlohmann::json j;
        j["schema_version"] = 1;
        j["event_type"] = "book_state";
        j["source"] = source;
        j["venue"] = venue_;
        j["symbol"] = symbol_;
        j["persist_seq"] = ++persist_seq_;
        j["ts_recv_ns"] = 0;
        j["ts_book_ns"] = ts_book_ns;
        j["ts_persist_ns"] = now_ns_();
        j["applied_seq"] = applied_seq;
        j["top_n"] = top_n;
        j["bids"] = levels_from_book_(book, top_n, Side::BID);
        j["asks"] = levels_from_book_(book, top_n, Side::ASK);
        send_json_(j);
    }
} // namespace md
