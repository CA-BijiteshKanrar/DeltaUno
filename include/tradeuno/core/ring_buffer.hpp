#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <vector>
#include <optional>
#include <type_traits>

namespace tradeuno::core {

#ifdef __cpp_lib_hardware_interference_size
    using std::hardware_destructive_interference_size;
#else
    // Standard L1 cache line size on x86/x64
    constexpr size_t hardware_destructive_interference_size = 64;
#endif

template <typename T, size_t Capacity = 65536>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable for hot-path ring buffer");

public:
    SPSCRingBuffer() = default;

    ~SPSCRingBuffer() = default;

    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer(SPSCRingBuffer&&) = delete;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&) = delete;

    [[nodiscard]] bool push(const T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (head + 1) & BufferMask;

        if (next_head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (next_head == tail_cache_) {
                return false; // Buffer full
            }
        }

        buffer_[head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> pop() noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail == head_cache_) {
                return std::nullopt; // Buffer empty
            }
        }

        T item = buffer_[tail];
        tail_.store((tail + 1) & BufferMask, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (Capacity - (tail - head));
    }

private:
    static constexpr size_t BufferMask = Capacity - 1;

    // Cache-aligned storage to prevent false sharing
    alignas(hardware_destructive_interference_size) std::array<T, Capacity> buffer_{};

    alignas(hardware_destructive_interference_size) std::atomic<size_t> head_{0};
    size_t tail_cache_{0};

    alignas(hardware_destructive_interference_size) std::atomic<size_t> tail_{0};
    size_t head_cache_{0};
};

} // namespace tradeuno::core
