#pragma once

#include <chrono>
#include <atomic>
#include <functional>
#include <thread>
#include <mutex>

namespace tradeuno::market {

using AlertCallback = std::function<void(const std::string&)>;

class Watchdog {
public:
    explicit Watchdog(uint32_t timeout_seconds = 5)
        : timeout_ns_(timeout_seconds * 1'000'000'000ULL) {}

    void register_alert_callback(AlertCallback cb) {
        callback_ = std::move(cb);
    }

    void heartbeat(uint64_t tick_time_ns = 0) noexcept {
        if (tick_time_ns == 0) {
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            last_heartbeat_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(), std::memory_order_relaxed);
        } else {
            last_heartbeat_ns_.store(tick_time_ns, std::memory_order_relaxed);
        }
    }

    void start_monitoring() {
        is_monitoring_ = true;
        heartbeat();
        monitor_thread_ = std::thread([this]() {
            while (is_monitoring_) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!is_monitoring_) break;

                auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                auto last = last_heartbeat_ns_.load(std::memory_order_relaxed);

                if (has_open_position_ && (now - last > timeout_ns_)) {
                    if (callback_) {
                        callback_("WATCHDOG_ALERT: No market tick received for over timeout duration with open position!");
                    }
                }
            }
        });
    }

    void set_has_open_position(bool has_pos) noexcept {
        has_open_position_ = has_pos;
    }

    void stop_monitoring() {
        is_monitoring_ = false;
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
    }

    ~Watchdog() {
        stop_monitoring();
    }

private:
    uint64_t timeout_ns_;
    std::atomic<uint64_t> last_heartbeat_ns_{0};
    std::atomic<bool> is_monitoring_{false};
    std::atomic<bool> has_open_position_{false};
    AlertCallback callback_;
    std::thread monitor_thread_;
};

} // namespace tradeuno::market
