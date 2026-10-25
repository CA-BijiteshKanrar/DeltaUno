#pragma once

#include "deltauno/core/types.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace deltauno::core {

struct RiskConfig {
    double initial_capital_inr{10000.0};
    double max_risk_per_trade_inr{120.0};
    double max_daily_drawdown_inr{500.0};
    double max_position_value_inr{5000.0};
    uint32_t max_open_positions{1};
    double breakeven_trigger_pct{1.0};
    double min_payoff_to_fee_ratio{2.5};

    // Strategy parameters for dual-mode optimization (Compounder vs Harvester)
    double take_profit_pct{1.75};
    double stop_loss_pct{0.85};
    double min_composite_ofi{0.55};
    double min_bid_depth_ratio{1.25};
    bool disable_afternoon_entries{false};
    bool enable_ofi_harvest{false};

    // Seconds from midnight IST
    int morning_start_sec{9 * 3600 + 30 * 60};     // 09:30:00
    int morning_end_sec{11 * 3600};                // 11:00:00
    int afternoon_start_sec{13 * 3600 + 30 * 60};  // 13:30:00
    int afternoon_end_sec{14 * 3600 + 45 * 60};    // 14:45:00
    int auto_squareoff_sec{15 * 3600 + 5 * 60};    // 15:05:00

    double circuit_limit_buffer_pct{1.5};
    double max_15s_volume_burst_multiple{4.0};
    uint32_t heartbeat_timeout_seconds{5};
};

struct InstrumentInfo {
    uint32_t token{0};
    std::string symbol;
    std::string exchange{"NSE"};
    Price tick_size_paise{5};
    Quantity lot_size{1};
    Price reference_price{0};
    Price upper_circuit{0};
    Price lower_circuit{0};
    uint64_t avg_daily_volume{0};
};

struct MacroEventWindow {
    std::string name;
    uint64_t start_utc_ns{0};
    uint64_t end_utc_ns{0};
};

class MacroCalendar {
public:
    void add_event(MacroEventWindow ev) {
        events_.push_back(std::move(ev));
    }

    // Sub-nanosecond integer range check on the hot path
    [[nodiscard]] bool is_frozen(uint64_t timestamp_ns) const noexcept {
        for (const auto& ev : events_) {
            if (timestamp_ns >= ev.start_utc_ns && timestamp_ns <= ev.end_utc_ns) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] size_t size() const noexcept {
        return events_.size();
    }

private:
    std::vector<MacroEventWindow> events_;
};

class ConfigLoader {
public:
    // Parses "HH:MM:SS" or "HH:MM" into seconds from midnight
    [[nodiscard]] static int parse_hms_seconds(std::string_view hms, int fallback = 0) noexcept {
        if (hms.size() < 5) return fallback;
        auto parse2 = [](std::string_view s, size_t off) -> int {
            if (off + 2 > s.size()) return -1;
            if (s[off] < '0' || s[off] > '9' || s[off + 1] < '0' || s[off + 1] > '9') return -1;
            return (s[off] - '0') * 10 + (s[off + 1] - '0');
        };
        const int hh = parse2(hms, 0);
        const int mm = parse2(hms, 3);
        const int ss = (hms.size() >= 8) ? parse2(hms, 6) : 0;
        if (hh < 0 || mm < 0 || ss < 0) return fallback;
        return hh * 3600 + mm * 60 + ss;
    }

    // Converts "YYYY-MM-DD" + "HH:MM:SS" in IST (UTC+05:30) to UTC epoch nanoseconds
    [[nodiscard]] static uint64_t ist_datetime_to_utc_ns(std::string_view ymd, std::string_view hms) noexcept {
        if (ymd.size() < 10) return 0;
        const int year = std::stoi(std::string(ymd.substr(0, 4)));
        const unsigned month = static_cast<unsigned>(std::stoi(std::string(ymd.substr(5, 2))));
        const unsigned day = static_cast<unsigned>(std::stoi(std::string(ymd.substr(8, 2))));

        const std::chrono::year_month_day civil{
            std::chrono::year{year},
            std::chrono::month{month},
            std::chrono::day{day}
        };
        if (!civil.ok()) return 0;

        const auto day_tp = std::chrono::sys_days{civil};
        const int64_t midnight_utc_sec = std::chrono::duration_cast<std::chrono::seconds>(day_tp.time_since_epoch()).count();
        const int64_t ist_sec_from_midnight = parse_hms_seconds(hms, 0);
        constexpr int64_t kIstOffsetSec = 5 * 3600 + 30 * 60; // +19800s

        const int64_t utc_sec = midnight_utc_sec + ist_sec_from_midnight - kIstOffsetSec;
        return (utc_sec > 0) ? (static_cast<uint64_t>(utc_sec) * 1'000'000'000ULL) : 0ULL;
    }

    [[nodiscard]] static RiskConfig load_risk_config(const std::string& path) {
        RiskConfig cfg{};
        const std::string content = read_text(path);
        if (content.empty()) return cfg;

        if (auto v = extract_number(content, "initial_capital_inr")) cfg.initial_capital_inr = *v;
        if (auto v = extract_number(content, "max_risk_per_trade_inr")) cfg.max_risk_per_trade_inr = *v;
        if (auto v = extract_number(content, "max_daily_drawdown_inr")) cfg.max_daily_drawdown_inr = *v;
        if (auto v = extract_number(content, "max_position_value_inr")) cfg.max_position_value_inr = *v;
        if (auto v = extract_number(content, "max_open_positions")) cfg.max_open_positions = static_cast<uint32_t>(*v);
        if (auto v = extract_number(content, "breakeven_trigger_pct")) cfg.breakeven_trigger_pct = *v;
        if (auto v = extract_number(content, "min_payoff_to_fee_ratio")) cfg.min_payoff_to_fee_ratio = *v;

        if (auto s = extract_string(content, "trading_window_morning_start")) {
            cfg.morning_start_sec = parse_hms_seconds(*s, cfg.morning_start_sec);
        }
        if (auto s = extract_string(content, "trading_window_morning_end")) {
            cfg.morning_end_sec = parse_hms_seconds(*s, cfg.morning_end_sec);
        }
        if (auto s = extract_string(content, "trading_window_afternoon_start")) {
            cfg.afternoon_start_sec = parse_hms_seconds(*s, cfg.afternoon_start_sec);
        }
        if (auto s = extract_string(content, "trading_window_afternoon_end")) {
            cfg.afternoon_end_sec = parse_hms_seconds(*s, cfg.afternoon_end_sec);
        }
        if (auto s = extract_string(content, "auto_squareoff_cutoff")) {
            cfg.auto_squareoff_sec = parse_hms_seconds(*s, cfg.auto_squareoff_sec);
        }

        if (auto v = extract_number(content, "circuit_limit_buffer_pct")) cfg.circuit_limit_buffer_pct = *v;
        if (auto v = extract_number(content, "max_15s_volume_burst_multiple")) cfg.max_15s_volume_burst_multiple = *v;
        if (auto v = extract_number(content, "heartbeat_timeout_seconds")) {
            cfg.heartbeat_timeout_seconds = static_cast<uint32_t>(*v);
        }

        if (auto v = extract_number(content, "take_profit_pct")) cfg.take_profit_pct = *v;
        if (auto v = extract_number(content, "stop_loss_pct")) cfg.stop_loss_pct = *v;
        if (auto v = extract_number(content, "min_composite_ofi")) cfg.min_composite_ofi = *v;
        if (auto v = extract_number(content, "min_bid_depth_ratio")) cfg.min_bid_depth_ratio = *v;
        if (content.find("\"disable_afternoon_entries\": true") != std::string::npos ||
            content.find("\"disable_afternoon_entries\":true") != std::string::npos) {
            cfg.disable_afternoon_entries = true;
        } else if (content.find("\"disable_afternoon_entries\": false") != std::string::npos ||
                   content.find("\"disable_afternoon_entries\":false") != std::string::npos) {
            cfg.disable_afternoon_entries = false;
        }
        if (content.find("\"enable_ofi_harvest\": true") != std::string::npos ||
            content.find("\"enable_ofi_harvest\":true") != std::string::npos) {
            cfg.enable_ofi_harvest = true;
        } else if (content.find("\"enable_ofi_harvest\": false") != std::string::npos ||
                   content.find("\"enable_ofi_harvest\":false") != std::string::npos) {
            cfg.enable_ofi_harvest = false;
        }

        return cfg;
    }

    [[nodiscard]] static std::vector<InstrumentInfo> load_instruments(const std::string& path) {
        std::vector<InstrumentInfo> list;
        const std::string content = read_text(path);
        if (content.empty()) return default_instruments();

        size_t cursor = content.find("\"instruments\"");
        if (cursor == std::string::npos) return default_instruments();

        while (true) {
            const size_t obj_start = content.find('{', cursor);
            if (obj_start == std::string::npos) break;
            const size_t obj_end = content.find('}', obj_start);
            if (obj_end == std::string::npos) break;

            const std::string block = content.substr(obj_start, obj_end - obj_start + 1);
            if (auto sym = extract_string(block, "symbol")) {
                InstrumentInfo info{};
                info.symbol = *sym;
                if (auto tok = extract_number(block, "token")) info.token = static_cast<uint32_t>(*tok);
                if (auto ex = extract_string(block, "exchange")) info.exchange = *ex;
                if (auto ts = extract_number(block, "tick_size")) info.tick_size_paise = std::max<Price>(1, to_paise(*ts));
                if (auto lot = extract_number(block, "lot_size")) info.lot_size = static_cast<Quantity>(*lot);
                if (auto ref = extract_number(block, "reference_price_inr")) info.reference_price = to_paise(*ref);
                if (auto uc = extract_number(block, "upper_circuit_inr")) info.upper_circuit = to_paise(*uc);
                if (auto lc = extract_number(block, "lower_circuit_inr")) info.lower_circuit = to_paise(*lc);
                if (auto vol = extract_number(block, "avg_daily_volume")) info.avg_daily_volume = static_cast<uint64_t>(*vol);
                list.push_back(std::move(info));
            }
            cursor = obj_end + 1;
        }

        return list.empty() ? default_instruments() : list;
    }

    [[nodiscard]] static MacroCalendar load_macro_calendar(const std::string& path) {
        MacroCalendar cal;
        const std::string content = read_text(path);
        if (content.empty()) return cal;

        size_t cursor = content.find("\"macro_events\"");
        if (cursor == std::string::npos) return cal;

        while (true) {
            const size_t obj_start = content.find('{', cursor);
            if (obj_start == std::string::npos) break;
            const size_t obj_end = content.find('}', obj_start);
            if (obj_end == std::string::npos) break;

            const std::string block = content.substr(obj_start, obj_end - obj_start + 1);
            const auto name = extract_string(block, "name");
            const auto date = extract_string(block, "date");
            const auto start_s = extract_string(block, "freeze_start");
            const auto end_s = extract_string(block, "freeze_end");

            if (date && start_s && end_s) {
                MacroEventWindow ev{};
                ev.name = name.value_or("Macro Event");
                ev.start_utc_ns = ist_datetime_to_utc_ns(*date, *start_s);
                ev.end_utc_ns = ist_datetime_to_utc_ns(*date, *end_s);
                if (ev.start_utc_ns > 0 && ev.end_utc_ns >= ev.start_utc_ns) {
                    cal.add_event(std::move(ev));
                }
            }
            cursor = obj_end + 1;
        }

        return cal;
    }

    [[nodiscard]] static std::unordered_set<std::string> to_whitelist_set(const std::vector<InstrumentInfo>& instruments) {
        std::unordered_set<std::string> out;
        for (const auto& inst : instruments) {
            out.insert(inst.symbol);
        }
        return out;
    }

    [[nodiscard]] static std::vector<InstrumentInfo> default_instruments() {
        return {
            {349121, "TATASTEEL", "NSE", 5, 1, to_paise(150.0), to_paise(165.0), to_paise(135.0), 45'000'000},
            {779521, "SBIN",      "NSE", 5, 1, to_paise(800.0), to_paise(880.0), to_paise(720.0), 18'000'000},
            {424961, "ITC",       "NSE", 5, 1, to_paise(480.0), to_paise(528.0), to_paise(432.0), 14'000'000},
            {521729, "COALINDIA", "NSE", 5, 1, to_paise(460.0), to_paise(506.0), to_paise(414.0), 12'000'000},
            {895745, "ZOMATO",    "NSE", 5, 1, to_paise(250.0), to_paise(275.0), to_paise(225.0), 35'000'000},
        };
    }

    [[nodiscard]] static std::optional<double> extract_number(std::string_view json, std::string_view key) {
        const std::string needle = "\"" + std::string(key) + "\"";
        const size_t kpos = json.find(needle);
        if (kpos == std::string_view::npos) return std::nullopt;
        const size_t colon = json.find(':', kpos + needle.size());
        if (colon == std::string_view::npos) return std::nullopt;
        const size_t start = json.find_first_not_of(" \t\r\n", colon + 1);
        if (start == std::string_view::npos) return std::nullopt;
        const size_t end = json.find_first_of(",}\r\n \t", start);
        try {
            return std::stod(std::string(json.substr(start, end - start)));
        } catch (...) {
            return std::nullopt;
        }
    }

    [[nodiscard]] static std::optional<std::string> extract_string(std::string_view json, std::string_view key) {
        const std::string needle = "\"" + std::string(key) + "\"";
        const size_t kpos = json.find(needle);
        if (kpos == std::string_view::npos) return std::nullopt;
        const size_t colon = json.find(':', kpos + needle.size());
        if (colon == std::string_view::npos) return std::nullopt;
        const size_t q1 = json.find('"', colon + 1);
        if (q1 == std::string_view::npos) return std::nullopt;
        const size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return std::nullopt;
        return std::string(json.substr(q1 + 1, q2 - q1 - 1));
    }

private:
    static std::string read_text(const std::string& path) {
        std::ifstream in(path);
        if (!in.is_open()) return "";
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    }
};

} // namespace deltauno::core
