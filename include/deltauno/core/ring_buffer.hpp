#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace deltauno::core {

constexpr size_t kCacheLineSize = 64;

template <typename T, size_t Capacity = 4096>
class SPSCRingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two >= 2");
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
        const size_t next_head = (head + 1) & kBufferMask;

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
        tail_.store((tail + 1) & kBufferMask, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & kBufferMask;
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity - 1;
    }

private:
    static constexpr size_t kBufferMask = Capacity - 1;

    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};

    alignas(kCacheLineSize) std::atomic<size_t> head_{0};
    size_t tail_cache_{0};

    alignas(kCacheLineSize) std::atomic<size_t> tail_{0};
    size_t head_cache_{0};
};

// Bounded lock-free Vyukov MPMC ring buffer for multi-producer telemetry/logging
template <typename T, size_t Capacity = 4096>
class MPMCRingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two >= 2");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

public:
    MPMCRingBuffer() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MPMCRingBuffer(const MPMCRingBuffer&) = delete;
    MPMCRingBuffer& operator=(const MPMCRingBuffer&) = delete;

    [[nodiscard]] bool push(const T& item) noexcept {
        Cell* cell = nullptr;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kBufferMask];
            const size_t seq = cell->sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // Full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = item;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> pop() noexcept {
        Cell* cell = nullptr;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kBufferMask];
            const size_t seq = cell->sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return std::nullopt; // Empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        T item = cell->data;
        cell->sequence.store(pos + kBufferMask + 1, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept {
        const size_t pos = dequeue_pos_.load(std::memory_order_acquire);
        const size_t seq = cells_[pos & kBufferMask].sequence.load(std::memory_order_acquire);
        return static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1) < 0;
    }

private:
    static constexpr size_t kBufferMask = Capacity - 1;

    struct Cell {
        std::atomic<size_t> sequence{0};
        T data{};
    };

    alignas(kCacheLineSize) std::array<Cell, Capacity> cells_{};
    alignas(kCacheLineSize) std::atomic<size_t> enqueue_pos_{0};
    alignas(kCacheLineSize) std::atomic<size_t> dequeue_pos_{0};
};

} // namespace deltauno::core
