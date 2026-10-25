#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace deltauno::market {

using AlertCallback = std::function<void(const std::string&)>;

// Dual-mode network heartbeat monitor:
//  1) Live wall-clock stall monitoring via std::chrono::steady_clock (never mixed with epoch timestamps)
//  2) Deterministic tick-timestamp gap detection for 100% backtest-to-live replay parity
class Watchdog {
public:
    explicit Watchdog(uint32_t timeout_seconds = 5)
        : timeout_ns_(static_cast<uint64_t>(timeout_seconds) * 1'000'000'000ULL) {}

    void register_alert_callback(AlertCallback cb) {
        callback_ = std::move(cb);
    }

    // Updates live wall-clock heartbeat AND checks replay tick-stream gaps deterministically
    void on_tick(uint64_t tick_timestamp_ns) {
        heartbeat();

        const uint64_t prev_tick_ns = last_tick_timestamp_ns_.exchange(tick_timestamp_ns, std::memory_order_acq_rel);
        if (prev_tick_ns > 0 && tick_timestamp_ns > prev_tick_ns) {
            const uint64_t gap_ns = tick_timestamp_ns - prev_tick_ns;
            if (has_open_position_.load(std::memory_order_acquire) && gap_ns > timeout_ns_) {
                alert_count_.fetch_add(1, std::memory_order_relaxed);
                if (callback_) {
                    callback_("WATCHDOG_ALERT: Market tick gap exceeded timeout (" +
                              std::to_string(gap_ns / 1'000'000'000ULL) + "s) while holding an open position!");
                }
            }
        }
    }

    void heartbeat() noexcept {
        last_steady_ns_.store(now_steady_ns(), std::memory_order_release);
    }

    void start_monitoring() {
        bool expected = false;
        if (!is_monitoring_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        heartbeat();
        monitor_thread_ = std::thread([this]() {
            while (is_monitoring_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (!is_monitoring_.load(std::memory_order_acquire)) break;

                const uint64_t now_ns = now_steady_ns();
                const uint64_t last_ns = last_steady_ns_.load(std::memory_order_acquire);

                if (has_open_position_.load(std::memory_order_acquire) &&
                    now_ns > last_ns &&
                    (now_ns - last_ns) > timeout_ns_) {
                    alert_count_.fetch_add(1, std::memory_order_relaxed);
                    if (callback_) {
                        callback_("WATCHDOG_ALERT: No live market tick received within timeout window while holding an open position!");
                    }
                    // Reset timer after alert to rate-limit once per timeout window
                    last_steady_ns_.store(now_ns, std::memory_order_release);
                }
            }
        });
    }

    void set_has_open_position(bool has_pos) noexcept {
        has_open_position_.store(has_pos, std::memory_order_release);
    }

    void stop_monitoring() {
        bool expected = true;
        if (!is_monitoring_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;
        }
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
    }

    [[nodiscard]] uint64_t alert_count() const noexcept {
        return alert_count_.load(std::memory_order_relaxed);
    }

    ~Watchdog() {
        stop_monitoring();
    }

private:
    [[nodiscard]] static uint64_t now_steady_ns() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    uint64_t timeout_ns_;
    std::atomic<uint64_t> last_steady_ns_{0};
    std::atomic<uint64_t> last_tick_timestamp_ns_{0};
    std::atomic<uint64_t> alert_count_{0};
    std::atomic<bool> is_monitoring_{false};
    std::atomic<bool> has_open_position_{false};
    AlertCallback callback_;
    std::thread monitor_thread_;
};

} // namespace deltauno::market
