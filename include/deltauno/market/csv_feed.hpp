#pragma once

#include "deltauno/market/feed_interface.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace deltauno::market {

class CSVFeed : public IMarketFeed {
public:
    explicit CSVFeed(std::string csv_path,
                     bool simulate_realtime_delay = false,
                     core::Price tick_size_paise = 5,
                     bool live_tail = false)
        : csv_path_(std::move(csv_path)),
          simulate_delay_(simulate_realtime_delay),
          tick_size_paise_(tick_size_paise),
          live_tail_(live_tail) {}

    void register_callback(TickCallback cb) override {
        callback_ = std::move(cb);
    }

    void register_alert_callback(ExternalAlertCallback cb) {
        alert_callback_ = std::move(cb);
    }

    bool start() override {
        std::ifstream file(csv_path_);
        if (!file.is_open()) return false;

        is_running_.store(true, std::memory_order_release);
        std::string line;
        uint64_t prev_ts = 0;

        while (is_running_.load(std::memory_order_acquire)) {
            const std::streampos line_start = file.tellg();
            if (!std::getline(file, line)) {
                if (live_tail_ && is_running_.load(std::memory_order_acquire)) {
                    file.clear();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                break;
            }

            // In live_tail_ mode, if EOF was hit mid-line (before '\n' was flushed), rewind and wait
            if (live_tail_ && file.eof() && line_start != std::streampos(-1)) {
                file.clear();
                file.seekg(line_start);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) continue;

            // Clean session termination sentinel for live-tail streaming
            if (line.rfind("#SESSION_END", 0) == 0 || line.rfind("#EOF", 0) == 0) {
                break;
            }

            // Optional embedded external alert rows: #ALERT,timestamp_ns,token,symbol,BUY,entry_inr,sl_inr
            if (line.rfind("#ALERT,", 0) == 0) {
                if (alert_callback_) {
                    alert_callback_(parse_alert_line(line.substr(7)));
                }
                continue;
            }

            if (line[0] == '#') continue;
            if (line.find("timestamp") != std::string::npos || line.find("symbol") != std::string::npos) {
                continue; // Header row
            }

            const core::MarketTick tick = parse_tick_line(line);

            if (simulate_delay_ && prev_ts != 0 && tick.timestamp_ns > prev_ts) {
                const uint64_t diff_ns = tick.timestamp_ns - prev_ts;
                if (diff_ns < 1'000'000'000ULL) {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(diff_ns));
                }
            }
            prev_ts = tick.timestamp_ns;

            if (callback_) {
                callback_(tick);
            }
        }

        is_running_.store(false, std::memory_order_release);
        return true;
    }

    void stop() override {
        is_running_.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool is_connected() const override {
        return is_running_.load(std::memory_order_acquire);
    }

private:
    [[nodiscard]] core::MarketTick parse_tick_line(const std::string& line) const {
        // Format: timestamp_ns,token,symbol,last_price_inr,last_qty,volume,bid0_p,bid0_q,ask0_p,ask0_q,upper_c,lower_c
        core::MarketTick tick{};
        std::stringstream ss(line);

        auto get_next = [&ss]() -> std::string {
            std::string val;
            if (std::getline(ss, val, ',')) {
                return val;
            }
            return "";
        };

        const std::string ts_str = get_next();
        if (!ts_str.empty()) tick.timestamp_ns = std::stoull(ts_str);

        const std::string tok_str = get_next();
        if (!tok_str.empty()) tick.token = static_cast<uint32_t>(std::stoul(tok_str));

        const std::string sym = get_next();
        core::copy_fixed_str(tick.symbol, sym);

        const std::string ltp_str = get_next();
        if (!ltp_str.empty()) tick.last_price = core::to_paise(std::stod(ltp_str));

        const std::string lq_str = get_next();
        if (!lq_str.empty()) tick.last_quantity = static_cast<core::Quantity>(std::stoul(lq_str));

        const std::string vol_str = get_next();
        if (!vol_str.empty()) tick.total_volume = std::stoull(vol_str);

        const std::string b0_p = get_next();
        const std::string b0_q = get_next();
        const std::string a0_p = get_next();
        const std::string a0_q = get_next();

        if (!b0_p.empty()) tick.bids[0].price = core::to_paise(std::stod(b0_p));
        if (!b0_q.empty()) tick.bids[0].quantity = static_cast<core::Quantity>(std::stoul(b0_q));
        if (!a0_p.empty()) tick.asks[0].price = core::to_paise(std::stod(a0_p));
        if (!a0_q.empty()) tick.asks[0].quantity = static_cast<core::Quantity>(std::stoul(a0_q));

        const std::string uc_str = get_next();
        const std::string lc_str = get_next();
        if (!uc_str.empty()) tick.upper_circuit = core::to_paise(std::stod(uc_str));
        if (!lc_str.empty()) tick.lower_circuit = core::to_paise(std::stod(lc_str));

        // Parse optional real L2 levels 1-4 (bid1_p,bid1_q,ask1_p,ask1_q,...,bid4_p,bid4_q,ask4_p,ask4_q) if present;
        // otherwise synthesize depth levels 1-4 separated by standard NSE tick size when CSV carries top-of-book
        for (size_t i = 1; i < 5; ++i) {
            const std::string bi_p = get_next();
            const std::string bi_q = get_next();
            const std::string ai_p = get_next();
            const std::string ai_q = get_next();
            if (!bi_p.empty() && !bi_q.empty() && !ai_p.empty() && !ai_q.empty()) {
                tick.bids[i].price = core::to_paise(std::stod(bi_p));
                tick.bids[i].quantity = static_cast<core::Quantity>(std::stoul(bi_q));
                tick.asks[i].price = core::to_paise(std::stod(ai_p));
                tick.asks[i].quantity = static_cast<core::Quantity>(std::stoul(ai_q));
            } else {
                const core::Price step = static_cast<core::Price>(i) * tick_size_paise_;
                tick.bids[i].price = std::max<core::Price>(0, tick.bids[0].price - step);
                tick.bids[i].quantity = tick.bids[0].quantity + static_cast<core::Quantity>(i * 100);
                tick.asks[i].price = tick.asks[0].price + step;
                tick.asks[i].quantity = tick.asks[0].quantity + static_cast<core::Quantity>(i * 100);
            }
        }

        return tick;
    }

    [[nodiscard]] static core::ExternalAlert parse_alert_line(const std::string& payload) {
        core::ExternalAlert alert{};
        std::stringstream ss(payload);
        auto get_next = [&ss]() -> std::string {
            std::string val;
            return std::getline(ss, val, ',') ? val : "";
        };

        const std::string ts_str = get_next();
        if (!ts_str.empty()) alert.timestamp_ns = std::stoull(ts_str);

        const std::string tok_str = get_next();
        if (!tok_str.empty()) alert.token = static_cast<uint32_t>(std::stoul(tok_str));

        core::copy_fixed_str(alert.symbol, get_next());

        const std::string side_str = get_next();
        alert.side = (side_str == "SELL") ? core::Side::SELL : core::Side::BUY;

        const std::string entry_str = get_next();
        if (!entry_str.empty()) alert.suggested_entry = core::to_paise(std::stod(entry_str));

        const std::string sl_str = get_next();
        if (!sl_str.empty()) alert.suggested_sl = core::to_paise(std::stod(sl_str));

        return alert;
    }

    std::string csv_path_;
    bool simulate_delay_{false};
    core::Price tick_size_paise_{5};
    bool live_tail_{false};
    std::atomic<bool> is_running_{false};
    TickCallback callback_;
    ExternalAlertCallback alert_callback_;
};

} // namespace deltauno::market
