#include "tradeuno/market/order_book.hpp"
#include <cassert>
#include <iostream>

using namespace tradeuno;

void test_order_book_basics() {
    market::OrderBook book(349121, "TATASTEEL");
    assert(book.best_bid() == 0);
    assert(book.best_ask() == 0);

    core::MarketTick tick{};
    tick.token = 349121;
    tick.timestamp_ns = 1730002200000000000ULL;
    tick.last_price = core::to_paise(150.00);
    tick.upper_circuit = core::to_paise(165.00);
    tick.lower_circuit = core::to_paise(135.00);

    tick.bids[0] = {core::to_paise(149.95), 1000, 5};
    tick.bids[1] = {core::to_paise(149.90), 2000, 10};
    tick.asks[0] = {core::to_paise(150.05), 800, 4};
    tick.asks[1] = {core::to_paise(150.10), 1500, 8};

    book.on_tick(tick);

    assert(book.best_bid() == core::to_paise(149.95));
    assert(book.best_ask() == core::to_paise(150.05));
    assert(book.spread() == core::to_paise(0.10));
    assert(book.mid_price() == core::to_paise(150.00));
    assert(!book.is_near_circuit(1.5));

    // Next tick with aggressive buyer queue addition (OFI surge)
    core::MarketTick tick2 = tick;
    tick2.timestamp_ns += 1'000'000'000ULL;
    tick2.bids[0] = {core::to_paise(150.00), 2500, 12}; // Bids push higher and size doubles
    tick2.asks[0] = {core::to_paise(150.05), 400, 2};  // Asks get depleted

    book.on_tick(tick2);

    assert(book.best_bid() == core::to_paise(150.00));
    // OFI should be strongly positive
    assert(book.composite_ofi() > 0.30);

    std::cout << "[PASS] test_order_book_basics\n";
}

void test_circuit_guard() {
    market::OrderBook book(349121, "TATASTEEL");
    core::MarketTick tick{};
    tick.last_price = core::to_paise(163.50); // within 1.0% of 165.00 upper circuit
    tick.upper_circuit = core::to_paise(165.00);
    tick.lower_circuit = core::to_paise(135.00);

    book.on_tick(tick);
    assert(book.is_near_circuit(1.5) == true);

    std::cout << "[PASS] test_circuit_guard\n";
}

int main() {
    std::cout << "Running OrderBook tests...\n";
    test_order_book_basics();
    test_circuit_guard();
    std::cout << "All OrderBook tests passed!\n";
    return 0;
}
