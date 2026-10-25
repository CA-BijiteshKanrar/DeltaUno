#pragma once

#include "deltauno/core/types.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>

namespace deltauno::core {

class StateJournal {
public:
    explicit StateJournal(std::string journal_path = "deltauno_state.json")
        : journal_path_(std::move(journal_path)) {}

    // Crash-safe atomic persistence via temporary file + filesystem rename
    bool save_position(const Position& pos) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string tmp_path = journal_path_ + ".tmp";

        {
            std::ofstream out(tmp_path, std::ios::out | std::ios::trunc);
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
            if (out.fail()) return false;
        }

        return atomic_replace(tmp_path, journal_path_);
    }

    bool clear_position() {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string tmp_path = journal_path_ + ".tmp";

        {
            std::ofstream out(tmp_path, std::ios::out | std::ios::trunc);
            if (!out.is_open()) return false;
            out << "{\n  \"has_position\": false\n}\n";
            out.flush();
            if (out.fail()) return false;
        }

        return atomic_replace(tmp_path, journal_path_);
    }

    [[nodiscard]] std::optional<Position> recover_position() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ifstream in(journal_path_);
        if (!in.is_open()) return std::nullopt;

        const std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());

        if (content.find("\"has_position\": true") == std::string::npos) {
            return std::nullopt;
        }

        auto extract_int = [&](const std::string& key) -> int64_t {
            const auto pos_k = content.find("\"" + key + "\":");
            if (pos_k == std::string::npos) return 0;
            const auto val_start = content.find_first_not_of(" \t", pos_k + key.length() + 3);
            if (val_start == std::string::npos) return 0;
            const auto val_end = content.find_first_of(",\n\r}", val_start);
            return std::stoll(content.substr(val_start, val_end - val_start));
        };

        auto extract_uint = [&](const std::string& key) -> uint64_t {
            const auto pos_k = content.find("\"" + key + "\":");
            if (pos_k == std::string::npos) return 0;
            const auto val_start = content.find_first_not_of(" \t", pos_k + key.length() + 3);
            if (val_start == std::string::npos) return 0;
            const auto val_end = content.find_first_of(",\n\r}", val_start);
            return std::stoull(content.substr(val_start, val_end - val_start));
        };

        auto extract_string = [&](const std::string& key) -> std::string {
            const auto pos_k = content.find("\"" + key + "\":");
            if (pos_k == std::string::npos) return "";
            const auto q1 = content.find('"', pos_k + key.length() + 3);
            if (q1 == std::string::npos) return "";
            const auto q2 = content.find('"', q1 + 1);
            if (q2 == std::string::npos) return "";
            return content.substr(q1 + 1, q2 - q1 - 1);
        };

        Position pos{};
        pos.token = static_cast<uint32_t>(extract_uint("token"));
        copy_fixed_str(pos.symbol, extract_string("symbol"));
        pos.side = static_cast<Side>(extract_int("side"));
        pos.quantity = static_cast<Quantity>(extract_uint("quantity"));
        pos.avg_entry_price = extract_int("avg_entry_price");
        pos.current_price = extract_int("current_price");
        pos.current_stop_loss = extract_int("current_stop_loss");
        pos.initial_stop_loss = extract_int("initial_stop_loss");
        pos.take_profit = extract_int("take_profit");
        pos.is_breakeven_set = (content.find("\"is_breakeven_set\": true") != std::string::npos);
        pos.peak_favorable_price = extract_int("peak_favorable_price");
        pos.worst_adverse_price = extract_int("worst_adverse_price");
        pos.open_time_ns = extract_uint("open_time_ns");

        if (pos.quantity == 0 || pos.avg_entry_price <= 0) {
            return std::nullopt;
        }
        return pos;
    }

private:
    static bool atomic_replace(const std::string& tmp_path, const std::string& target_path) noexcept {
        std::error_code ec;
#if defined(_WIN32) || defined(_WIN64)
        // On Windows std::filesystem::rename may fail if target exists depending on stdlib implementation
        std::filesystem::rename(tmp_path, target_path, ec);
        if (ec) {
            ec.clear();
            std::filesystem::remove(target_path, ec);
            ec.clear();
            std::filesystem::rename(tmp_path, target_path, ec);
        }
#else
        std::filesystem::rename(tmp_path, target_path, ec);
#endif
        return !ec;
    }

    std::string journal_path_;
    std::mutex mutex_;
};

} // namespace deltauno::core
