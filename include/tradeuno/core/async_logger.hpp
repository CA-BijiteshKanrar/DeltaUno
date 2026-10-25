#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>
#include <sstream>
#include <iomanip>

namespace tradeuno::core {

enum class LogLevel : uint8_t {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    TRADE,
    AUDIT
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

class AsyncLogger {
public:
    static AsyncLogger& instance() {
        static AsyncLogger logger;
        return logger;
    }

    void init(const std::string& log_file_path) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (is_running_) return;

        file_stream_.open(log_file_path, std::ios::out | std::ios::app);
        is_running_ = true;
        worker_thread_ = std::thread(&AsyncLogger::process_logs, this);
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!is_running_) return;
            is_running_ = false;
        }
        cv_.notify_one();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        if (file_stream_.is_open()) {
            file_stream_.flush();
            file_stream_.close();
        }
    }

    void log(LogLevel level, std::string message) {
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm local_tm{};
#if defined(_WIN32) || defined(_WIN64)
        localtime_s(&local_tm, &now_time_t);
#else
        localtime_r(&now_time_t, &local_tm);
#endif

        std::ostringstream ss;
        ss << "[" << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S")
           << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
           << "[" << log_level_to_string(level) << "] "
           << message;

        std::string formatted = ss.str();

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            log_queue_.push(std::move(formatted));
        }
        cv_.notify_one();
    }

    ~AsyncLogger() {
        shutdown();
    }

private:
    AsyncLogger() = default;

    void process_logs() {
        while (true) {
            std::string entry;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait(lock, [this] { return !log_queue_.empty() || !is_running_; });

                if (!is_running_ && log_queue_.empty()) {
                    break;
                }

                if (!log_queue_.empty()) {
                    entry = std::move(log_queue_.front());
                    log_queue_.pop();
                }
            }

            if (!entry.empty()) {
                std::cout << entry << "\n";
                if (file_stream_.is_open()) {
                    file_stream_ << entry << "\n";
                }
            }
        }
    }

    std::ofstream file_stream_;
    std::queue<std::string> log_queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> is_running_{false};
    std::thread worker_thread_;
};

#define LOG_DEBUG(msg) tradeuno::core::AsyncLogger::instance().log(tradeuno::core::LogLevel::DEBUG, msg)
#define LOG_INFO(msg)  tradeuno::core::AsyncLogger::instance().log(tradeuno::core::LogLevel::INFO,  msg)
#define LOG_WARN(msg)  tradeuno::core::AsyncLogger::instance().log(tradeuno::core::LogLevel::WARN,  msg)
#define LOG_ERROR(msg) tradeuno::core::AsyncLogger::instance().log(tradeuno::core::LogLevel::ERROR, msg)
#define LOG_TRADE(msg) tradeuno::core::AsyncLogger::instance().log(tradeuno::core::LogLevel::TRADE, msg)
#define LOG_AUDIT(msg) tradeuno::core::AsyncLogger::instance().log(tradeuno::core::LogLevel::AUDIT, msg)

} // namespace tradeuno::core
