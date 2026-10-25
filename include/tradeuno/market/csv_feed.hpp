#pragma once

#include "tradeuno/market/feed_interface.hpp"
#include <string>
#include <fstream>
#include <sstream>
#include <atomic>
#include <thread>
#include <chrono>

namespace tradeuno::market {

class CSVFeed : public IMarketFeed {
public:
    explicit CSVFeed(std::string csv_path, bool simulate_realtime_delay = false)
        : csv_path_(std::move(csv_path)), simulate_delay_(simulate_realtime_delay) {}

    void register_callback(TickCallback cb) override {
        callback_ = std::move(cb);
    }

    bool start() override {
        std::ifstream file(csv_path_);
        if (!file.is_open()) return false;

        is_running_ = true;
        std::string line;
        // Skip header if present
        if (std::getline(file, line)) {
            if (line.find("timestamp") == std::string::npos && line.find("token") == std::string::npos) {
                // Not a header, process it
                process_line(line);
            }
        }

        uint64_t prev_ts = 0;
        while (is_running_ && std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            core::MarketTick tick = parse_line(line);

            if (simulate_delay_ && prev_ts != 0 && tick.timestamp_ns > prev_ts) {
                auto diff_ns = tick.timestamp_ns - prev_ts;
                if (diff_ns < 1'000'000'000ULL) { // Cap delay at 1s
                    std::this_thread::sleep_for(std::chrono::nanoseconds(diff_ns));
                }
            }
            prev_ts = tick.timestamp_ns;

            if (callback_) {
                callback_(tick);
            }
        }

        is_running_ = false;
        return true;
    }

    void stop() override {
        is_running_ = false;
    }

    [[nodiscard]] bool is_connected() const override {
        return is_running_;
    }

private:
    void process_line(const std::string& line) {
        if (callback_) {
            callback_(parse_line(line));
        }
    }

    core::MarketTick parse_line(const std::string& line) {
        // Format: timestamp_ns,token,symbol,last_price_inr,last_qty,volume,bid0_p,bid0_q,ask0_p,ask0_q,upper_c,lower_c
        core::MarketTick tick{};
        std::stringstream ss(line);
        std::string token_str;

        auto get_next = [&ss]() -> std::string {
            std::string val;
            if (std::getline(ss, val, ',')) {
                return val;
            }
            return "";
        };

        std::string ts_str = get_next();
        if (!ts_str.empty()) tick.timestamp_ns = std::stoull(ts_str);

        std::string tok_str = get_next();
        if (!tok_str.empty()) tick.token = static_cast<uint32_t>(std::stoul(tok_str));

        std::string sym = get_next();
#if defined(_MSC_VER)
        strncpy_s(tick.symbol, sym.c_str(), sizeof(tick.symbol) - 1);
#else
        std::strncpy(tick.symbol, sym.c_str(), sizeof(tick.symbol) - 1);
#endif

        std::string ltp_str = get_next();
        if (!ltp_str.empty()) tick.last_price = core::to_paise(std::stod(ltp_str));

        std::string lq_str = get_next();
        if (!lq_str.empty()) tick.last_quantity = static_cast<core::Quantity>(std::stoul(lq_str));

        std::string vol_str = get_next();
        if (!vol_str.empty()) tick.total_volume = std::stoull(vol_str);

        // Bids & Asks Level 0
        std::string b0_p = get_next();
        std::string b0_q = get_next();
        std::string a0_p = get_next();
        std::string a0_q = get_next();

        if (!b0_p.empty()) tick.bids[0].price = core::to_paise(std::stod(b0_p));
        if (!b0_q.empty()) tick.bids[0].quantity = static_cast<core::Quantity>(std::stoul(b0_q));
        if (!a0_p.empty()) tick.asks[0].price = core::to_paise(std::stod(a0_p));
        if (!a0_q.empty()) tick.asks[0].quantity = static_cast<core::Quantity>(std::stoul(a0_q));

        // Circuit limits
        std::string uc_str = get_next();
        std::string lc_str = get_next();
        if (!uc_str.empty()) tick.upper_circuit = core::to_paise(std::stod(uc_str));
        if (!lc_str.empty()) tick.lower_circuit = core::to_paise(std::stod(lc_str));

        // Synthesize depth levels 1-4 for realism if not in simple CSV
        for (size_t i = 1; i < 5; ++i) {
            tick.bids[i].price = tick.bids[0].price - static_cast<core::Price>(i * 5); // 5 paise step
            tick.bids[i].quantity = tick.bids[0].quantity + static_cast<core::Quantity>(i * 100);
            tick.asks[i].price = tick.asks[0].price + static_cast<core::Price>(i * 5);
            tick.asks[i].quantity = tick.asks[0].quantity + static_cast<core::Quantity>(i * 100);
        }

        return tick;
    }

    std::string csv_path_;
    bool simulate_delay_{false};
    std::atomic<bool> is_running_{false};
    TickCallback callback_;
};

} // namespace tradeuno::market
