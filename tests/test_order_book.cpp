#include "deltauno/core/ring_buffer.hpp"
#include "deltauno/market/order_book.hpp"
#include "test_macros.hpp"

#include <iostream>
#include <memory>
#include <thread>

using namespace deltauno;

void test_order_book_basics_and_ofi() {
    market::OrderBook book(349121, "TATASTEEL");
    DU_EXPECT(book.best_bid() == 0);
    DU_EXPECT(book.best_ask() == 0);

    core::MarketTick tick{};
    tick.token = 349121;
    core::copy_fixed_str(tick.symbol, "TATASTEEL");
    tick.timestamp_ns = 1730002200000000000ULL;
    tick.last_price = core::to_paise(150.00);
    tick.upper_circuit = core::to_paise(165.00);
    tick.lower_circuit = core::to_paise(135.00);

    tick.bids[0] = {core::to_paise(149.95), 1000, 5};
    tick.bids[1] = {core::to_paise(149.90), 2000, 10};
    tick.asks[0] = {core::to_paise(150.05), 800, 4};
    tick.asks[1] = {core::to_paise(150.10), 1500, 8};

    book.on_tick(tick);

    DU_EXPECT(book.best_bid() == core::to_paise(149.95));
    DU_EXPECT(book.best_ask() == core::to_paise(150.05));
    DU_EXPECT(book.spread() == core::to_paise(0.10));
    DU_EXPECT(book.mid_price() == core::to_paise(150.00));
    // Bids heavier than asks -> microprice > mid_price (closer to ask)
    DU_EXPECT(book.weighted_mid_price() >= book.mid_price());
    DU_EXPECT(!book.is_near_circuit(1.5));

    // Next tick with aggressive buyer queue addition across top levels
    core::MarketTick tick2 = tick;
    tick2.timestamp_ns += 1'000'000'000ULL;
    tick2.bids[0] = {core::to_paise(150.00), 2500, 12};
    tick2.asks[0] = {core::to_paise(150.05), 400, 2};

    book.on_tick(tick2);

    DU_EXPECT(book.best_bid() == core::to_paise(150.00));
    DU_EXPECT(book.composite_ofi() > 0.55);

    std::cout << "[PASS] test_order_book_basics_and_ofi\n";
}

void test_circuit_guard_both_sides() {
    market::OrderBook book(349121, "TATASTEEL");
    core::MarketTick tick{};
    tick.last_price = core::to_paise(163.50); // within 1.0% of 165.00 upper circuit
    tick.upper_circuit = core::to_paise(165.00);
    tick.lower_circuit = core::to_paise(135.00);

    book.on_tick(tick);
    DU_EXPECT(book.is_near_circuit(1.5));

    tick.last_price = core::to_paise(136.50); // within 1.1% of 135.00 lower circuit
    book.on_tick(tick);
    DU_EXPECT(book.is_near_circuit(1.5));

    std::cout << "[PASS] test_circuit_guard_both_sides\n";
}

void test_spsc_ring_buffer_concurrency() {
    auto rb = std::make_unique<core::SPSCRingBuffer<uint64_t, 1024>>();
    constexpr uint64_t kCount = 20000;
    uint64_t sum_consumed = 0;

    std::thread producer([&]() {
        for (uint64_t i = 1; i <= kCount; ++i) {
            while (!rb->push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        uint64_t expected = 1;
        while (expected <= kCount) {
            if (auto v = rb->pop()) {
                DU_EXPECT(*v == expected);
                sum_consumed += *v;
                ++expected;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    DU_EXPECT(rb->empty());
    DU_EXPECT(sum_consumed == (kCount * (kCount + 1)) / 2);

    std::cout << "[PASS] test_spsc_ring_buffer_concurrency\n";
}

void test_order_book_session_reset() {
    market::OrderBook book(349121, "TATASTEEL");

    // Day 1 closing tick at 15:30:00 IST (150.00)
    core::MarketTick tick1{};
    tick1.token = 349121;
    core::copy_fixed_str(tick1.symbol, "TATASTEEL");
    tick1.timestamp_ns = 1730539800000000000ULL;
    tick1.last_price = core::to_paise(150.00);
    tick1.bids[0] = {core::to_paise(149.95), 1000, 5};
    tick1.asks[0] = {core::to_paise(150.05), 800, 4};
    book.on_tick(tick1);

    core::MarketTick tick2 = tick1;
    tick2.timestamp_ns += 1'000'000'000ULL;
    tick2.bids[0] = {core::to_paise(150.00), 2000, 8};
    tick2.asks[0] = {core::to_paise(150.05), 500, 2};
    book.on_tick(tick2);

    DU_EXPECT(book.composite_ofi() > 0.0);
    DU_EXPECT(book.has_previous_tick());
    DU_EXPECT(book.ofi_history_size() == 2);

    // Overnight rollover: Day 2 opening at 09:15:00 IST with +3.3% gap up to 155.00
    book.reset_session();

    DU_EXPECT(!book.has_previous_tick());
    DU_EXPECT(book.ofi_history_size() == 0);
    DU_EXPECT(book.best_bid() == 0);
    DU_EXPECT(book.best_ask() == 0);
    DU_EXPECT(book.composite_ofi() == 0.0);

    // Day 2 opening tick (09:15:00 IST)
    core::MarketTick tick_open{};
    tick_open.token = 349121;
    core::copy_fixed_str(tick_open.symbol, "TATASTEEL");
    tick_open.timestamp_ns = 1730603700000000000ULL;
    tick_open.last_price = core::to_paise(155.00);
    tick_open.bids[0] = {core::to_paise(154.95), 1200, 6};
    tick_open.asks[0] = {core::to_paise(155.05), 1100, 5};
    book.on_tick(tick_open);

    // Opening tick MUST have clean zero OFI, NOT an artificial +1.0 surge from the gap!
    DU_EXPECT(book.composite_ofi() == 0.0);
    DU_EXPECT(book.has_previous_tick());
    DU_EXPECT(book.ofi_history_size() == 1);
    DU_EXPECT(book.best_bid() == core::to_paise(154.95));
    DU_EXPECT(book.best_ask() == core::to_paise(155.05));

    // Subsequent tick on Day 2 reflects true intraday delta against tick_open
    core::MarketTick tick_day2_next = tick_open;
    tick_day2_next.timestamp_ns += 1'000'000'000ULL;
    tick_day2_next.bids[0] = {core::to_paise(155.00), 3000, 10};
    tick_day2_next.asks[0] = {core::to_paise(155.05), 800, 3};
    book.on_tick(tick_day2_next);

    DU_EXPECT(book.composite_ofi() > 0.0);

    std::cout << "[PASS] test_order_book_session_reset\n";
}

int main() {
    std::cout << "Running OrderBook & RingBuffer tests...\n";
    test_order_book_basics_and_ofi();
    test_circuit_guard_both_sides();
    test_spsc_ring_buffer_concurrency();
    test_order_book_session_reset();
    std::cout << "All OrderBook & RingBuffer tests passed!\n";
    return 0;
}
