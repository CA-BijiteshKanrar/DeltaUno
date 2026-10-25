#include "tradeuno/execution/paper_broker.hpp"
#include <cassert>
#include <iostream>

using namespace tradeuno;

void test_paper_matching_and_statutory_taxes() {
    execution::PaperBroker broker(1000.0);
    assert(broker.cash_balance_inr() == 1000.0);

    market::OrderBook book(349121, "TATASTEEL");
    core::MarketTick tick1{};
    tick1.token = 349121;
    tick1.timestamp_ns = 1000000;
    tick1.bids[0] = {core::to_paise(150.00), 1000, 5};
    tick1.asks[0] = {core::to_paise(150.10), 1000, 5};
    book.on_tick(tick1);

    // Place BUY order at tick 1
    core::Order buy_order{};
    buy_order.client_order_id = 1;
    buy_order.token = 349121;
#if defined(_MSC_VER)
    strncpy_s(buy_order.symbol, "TATASTEEL", sizeof(buy_order.symbol) - 1);
#else
    std::strncpy(buy_order.symbol, "TATASTEEL", sizeof(buy_order.symbol) - 1);
#endif
    buy_order.side = core::Side::BUY;
    buy_order.quantity = 3;
    buy_order.created_time_ns = 1000000;

    broker.place_order(buy_order);

    // If we call on_next_tick with SAME timestamp, it must NOT fill (Zero Look-Ahead Bias)
    broker.on_next_tick(tick1, book);
    assert(broker.cash_balance_inr() == 1000.0);

    // Tick 2 with higher timestamp
    core::MarketTick tick2 = tick1;
    tick2.timestamp_ns = 2000000;
    book.on_tick(tick2);

    broker.on_next_tick(tick2, book);
    // Order should now be filled!

    // Now place SELL order at tick 3
    core::Order sell_order = buy_order;
    sell_order.client_order_id = 2;
    sell_order.side = core::Side::SELL;
    sell_order.created_time_ns = 3000000;

    broker.place_order(sell_order);

    // Tick 4 with higher ask/bid: Bid pushes up to 152.00 (profitable trade)
    core::MarketTick tick4{};
    tick4.token = 349121;
    tick4.timestamp_ns = 4000000;
    tick4.bids[0] = {core::to_paise(152.00), 1000, 5};
    tick4.asks[0] = {core::to_paise(152.10), 1000, 5};
    book.on_tick(tick4);

    broker.on_next_tick(tick4, book);

    // Trade should be closed and taxes deducted
    assert(broker.total_trades_count() == 1);
    const auto& trade = broker.trade_history()[0];
    assert(trade.gross_pnl_inr > 0);
    assert(trade.stt_inr > 0);
    assert(trade.exchange_fee_inr > 0);
    assert(trade.gst_inr > 0);
    assert(trade.stamp_duty_inr > 0);
    assert(trade.net_pnl_inr < trade.gross_pnl_inr); // Taxes correctly reduced net profit!
    assert(broker.cash_balance_inr() > 1000.0); // Net profitable account!

    std::cout << "[PASS] test_paper_matching_and_statutory_taxes\n";
}

int main() {
    std::cout << "Running PaperBroker tests...\n";
    test_paper_matching_and_statutory_taxes();
    std::cout << "All PaperBroker tests passed!\n";
    return 0;
}
