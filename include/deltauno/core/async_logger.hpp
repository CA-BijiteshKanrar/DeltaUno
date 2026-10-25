#pragma once

#include "deltauno/core/ring_buffer.hpp"
#include "deltauno/core/types.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace deltauno::core {

enum class LogLevel : uint8_t {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
    TRADE = 4,
    AUDIT = 5
};

[[nodiscard]] constexpr const char* log_level_to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::TRADE: return "TRADE";
        case LogLevel::AUDIT: return "AUDIT";
        default: return "LOG  ";
    }
}

struct LogRecord {
    int64_t epoch_ms{0};
    LogLevel level{LogLevel::INFO};
    char message[384]{0};
};
static_assert(std::is_trivially_copyable_v<LogRecord>, "LogRecord must be trivially copyable");

// Lock-free ring-buffer logger: zero mutexes and zero stringstream formatting on the caller hot path
class AsyncLogger {
public:
    static AsyncLogger& instance() {
        static AsyncLogger logger;
        return logger;
    }

    void init(const std::string& log_file_path, LogLevel min_level = LogLevel::INFO) {
        bool expected = false;
        if (!is_running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        min_level_.store(static_cast<uint8_t>(min_level), std::memory_order_relaxed);
        file_stream_.open(log_file_path, std::ios::out | std::ios::app);
        worker_thread_ = std::thread(&AsyncLogger::process_logs, this);
    }

    void set_min_level(LogLevel level) noexcept {
        min_level_.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
    }

    [[nodiscard]] bool should_log(LogLevel level) const noexcept {
        return is_running_.load(std::memory_order_relaxed) &&
               static_cast<uint8_t>(level) >= min_level_.load(std::memory_order_relaxed);
    }

    void log(LogLevel level, std::string_view message) noexcept {
        if (!should_log(level)) {
            return;
        }

        LogRecord rec{};
        const auto now = std::chrono::system_clock::now();
        rec.epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        rec.level = level;
        copy_fixed_str(rec.message, message);

        pending_count_.fetch_add(1, std::memory_order_acq_rel);
        if (ring_->push(rec)) {
            signal_seq_.fetch_add(1, std::memory_order_release);
            signal_seq_.notify_one();
        } else {
            pending_count_.fetch_sub(1, std::memory_order_acq_rel);
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void flush() noexcept {
        if (!is_running_.load(std::memory_order_acquire)) return;
        while (pending_count_.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }
        std::cout.flush();
    }

    void shutdown() {
        bool expected = true;
        if (!is_running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;
        }
        signal_seq_.fetch_add(1, std::memory_order_release);
        signal_seq_.notify_all();

        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        if (file_stream_.is_open()) {
            file_stream_.flush();
            file_stream_.close();
        }
    }

    [[nodiscard]] uint64_t dropped_count() const noexcept {
        return dropped_count_.load(std::memory_order_relaxed);
    }

    ~AsyncLogger() {
        shutdown();
    }

private:
    AsyncLogger() : ring_(std::make_unique<MPMCRingBuffer<LogRecord, 4096>>()) {}

    static std::string format_record(const LogRecord& rec) {
        const std::time_t sec = static_cast<std::time_t>(rec.epoch_ms / 1000);
        const int64_t ms = rec.epoch_ms % 1000;

        std::tm local_tm{};
#if defined(_WIN32) || defined(_WIN64)
        localtime_s(&local_tm, &sec);
#else
        localtime_r(&sec, &local_tm);
#endif

        std::ostringstream ss;
        ss << "[" << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S")
           << "." << std::setfill('0') << std::setw(3) << ms << "] "
           << "[" << log_level_to_string(rec.level) << "] "
           << rec.message;
        return ss.str();
    }

    void drain_available() {
        while (auto opt = ring_->pop()) {
            const std::string formatted = format_record(*opt);
            std::cout << formatted << "\n";
            if (file_stream_.is_open()) {
                file_stream_ << formatted << "\n";
            }
            pending_count_.fetch_sub(1, std::memory_order_release);
        }
        if (file_stream_.is_open()) {
            file_stream_.flush();
        }
    }

    void process_logs() {
        while (true) {
            drain_available();

            if (!is_running_.load(std::memory_order_acquire) && ring_->empty()) {
                drain_available();
                break;
            }

            const uint64_t observed = signal_seq_.load(std::memory_order_acquire);
            if (ring_->empty() && is_running_.load(std::memory_order_acquire)) {
                signal_seq_.wait(observed, std::memory_order_relaxed);
            }
        }
    }

    std::unique_ptr<MPMCRingBuffer<LogRecord, 4096>> ring_;
    std::ofstream file_stream_;
    std::atomic<bool> is_running_{false};
    std::atomic<uint8_t> min_level_{static_cast<uint8_t>(LogLevel::INFO)};
    std::atomic<uint64_t> pending_count_{0};
    std::atomic<uint64_t> signal_seq_{0};
    std::atomic<uint64_t> dropped_count_{0};
    std::thread worker_thread_;
};

#define LOG_LEVEL_IMPL(lvl, msg_expr)                                           \
    do {                                                                        \
        auto& _du_logger = ::deltauno::core::AsyncLogger::instance();           \
        if (_du_logger.should_log(lvl)) {                                       \
            _du_logger.log((lvl), (msg_expr));                                  \
        }                                                                       \
    } while (0)

#define LOG_DEBUG(msg) LOG_LEVEL_IMPL(::deltauno::core::LogLevel::DEBUG, msg)
#define LOG_INFO(msg)  LOG_LEVEL_IMPL(::deltauno::core::LogLevel::INFO,  msg)
#define LOG_WARN(msg)  LOG_LEVEL_IMPL(::deltauno::core::LogLevel::WARN,  msg)
#define LOG_ERROR(msg) LOG_LEVEL_IMPL(::deltauno::core::LogLevel::ERROR, msg)
#define LOG_TRADE(msg) LOG_LEVEL_IMPL(::deltauno::core::LogLevel::TRADE, msg)
#define LOG_AUDIT(msg) LOG_LEVEL_IMPL(::deltauno::core::LogLevel::AUDIT, msg)

} // namespace deltauno::core
