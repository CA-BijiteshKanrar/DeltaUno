#pragma once

#include "tradeuno/core/types.hpp"
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <optional>
#include <mutex>

namespace tradeuno::core {

class StateJournal {
public:
    explicit StateJournal(std::string journal_path = "tradeuno_state.json")
        : journal_path_(std::move(journal_path)) {}

    bool save_position(const Position& pos) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream out(journal_path_, std::ios::out | std::ios::trunc);
        if (!out.is_open()) return false;

        out << "{\n";
        out << "  \"has_position\": true,\n";
        out << "  \"token\": " << pos.token << ",\n";
        out << "  \"symbol\": \"" << pos.symbol << "\",\n";
        out << "  \"side\": " << static_cast<int>(pos.side) << ",\n";
        out << "  \"quantity\": " << pos.quantity << ",\n";
        out << "  \"avg_entry_price\": " << pos.avg_entry_price << ",\n";
        out << "  \"current_price\": " << pos.current_price << ",\n";
        out << "  \"current_stop_loss\": " << pos.current_stop_loss << ",\n";
        out << "  \"initial_stop_loss\": " << pos.initial_stop_loss << ",\n";
        out << "  \"take_profit\": " << pos.take_profit << ",\n";
        out << "  \"is_breakeven_set\": " << (pos.is_breakeven_set ? "true" : "false") << ",\n";
        out << "  \"peak_favorable_price\": " << pos.peak_favorable_price << ",\n";
        out << "  \"worst_adverse_price\": " << pos.worst_adverse_price << ",\n";
        out << "  \"open_time_ns\": " << pos.open_time_ns << "\n";
        out << "}\n";

        out.flush();
        return true;
    }

    bool clear_position() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream out(journal_path_, std::ios::out | std::ios::trunc);
        if (!out.is_open()) return false;
        out << "{\n  \"has_position\": false\n}\n";
        out.flush();
        return true;
    }

    [[nodiscard]] std::optional<Position> recover_position() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ifstream in(journal_path_);
        if (!in.is_open()) return std::nullopt;

        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());

        if (content.find("\"has_position\": true") == std::string::npos) {
            return std::nullopt;
        }

        Position pos{};
        auto extract_int = [&](const std::string& key) -> int64_t {
            auto pos_k = content.find("\"" + key + "\":");
            if (pos_k == std::string::npos) return 0;
            auto val_start = content.find_first_not_of(" \t", pos_k + key.length() + 3);
            auto val_end = content.find_first_of(",\n\r}", val_start);
            return std::stoll(content.substr(val_start, val_end - val_start));
        };

        auto extract_string = [&](const std::string& key) -> std::string {
            auto pos_k = content.find("\"" + key + "\":");
            if (pos_k == std::string::npos) return "";
            auto q1 = content.find('"', pos_k + key.length() + 3);
            auto q2 = content.find('"', q1 + 1);
            return content.substr(q1 + 1, q2 - q1 - 1);
        };

        pos.token = static_cast<uint32_t>(extract_int("token"));
        std::string sym = extract_string("symbol");
        #if defined(_MSC_VER)
            strncpy_s(pos.symbol, sym.c_str(), sizeof(pos.symbol) - 1);
        #else
            std::strncpy(pos.symbol, sym.c_str(), sizeof(pos.symbol) - 1);
        #endif
        pos.side = static_cast<Side>(extract_int("side"));
        pos.quantity = static_cast<Quantity>(extract_int("quantity"));
        pos.avg_entry_price = extract_int("avg_entry_price");
        pos.current_price = extract_int("current_price");
        pos.current_stop_loss = extract_int("current_stop_loss");
        pos.initial_stop_loss = extract_int("initial_stop_loss");
        pos.take_profit = extract_int("take_profit");
        pos.is_breakeven_set = (content.find("\"is_breakeven_set\": true") != std::string::npos);
        pos.peak_favorable_price = extract_int("peak_favorable_price");
        pos.worst_adverse_price = extract_int("worst_adverse_price");
        pos.open_time_ns = extract_int("open_time_ns");

        return pos;
    }

private:
    std::string journal_path_;
    std::mutex mutex_;
};

} // namespace tradeuno::core
