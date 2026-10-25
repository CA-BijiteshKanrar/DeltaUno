#include "deltauno/execution/paper_broker.hpp"
#include "test_macros.hpp"

#include <iostream>

using namespace deltauno;

void test_paper_matching_excursions_and_statutory_taxes() {
    execution::PaperBroker broker(10000.0);
    DU_EXPECT_NEAR(broker.cash_balance_inr(), 10000.0, 1e-9);

    market::OrderBook book(349121, "TATASTEEL");
    core::MarketTick tick1{};
    tick1.token = 349121;
    tick1.timestamp_ns = 1000000;
    tick1.last_price = core::to_paise(150.05);
    tick1.bids[0] = {core::to_paise(150.00), 1000, 5};
    tick1.asks[0] = {core::to_paise(150.10), 1000, 5};
    book.on_tick(tick1);

    core::Order buy_order{};
    buy_order.client_order_id = 1;
    buy_order.token = 349121;
    core::copy_fixed_str(buy_order.symbol, "TATASTEEL");
    buy_order.side = core::Side::BUY;
    buy_order.quantity = 3;
    buy_order.created_time_ns = 1000000;

    DU_EXPECT(broker.place_order(buy_order));
    // Duplicate BUY while one is pending must be rejected
    DU_EXPECT(!broker.place_order(buy_order));

    // Same timestamp must NOT fill (Zero Look-Ahead Bias)
    broker.on_next_tick(tick1, book);
    DU_EXPECT(!broker.has_open_long());

    // Tick 2 fills BUY with adverse ceil slippage (>= 150.12)
    core::MarketTick tick2 = tick1;
    tick2.timestamp_ns = 2000000;
    book.on_tick(tick2);
    broker.on_next_tick(tick2, book);
    DU_EXPECT(broker.has_open_long());

    // Tick 3 dips adversely to 149.60 (records MAE), Tick 4 rallies to 152.50 (records MFE)
    core::MarketTick tick3 = tick1;
    tick3.timestamp_ns = 2500000;
    tick3.last_price = core::to_paise(149.60);
    tick3.bids[0] = {core::to_paise(149.60), 1000, 5};
    tick3.asks[0] = {core::to_paise(149.65), 1000, 5};
    book.on_tick(tick3);
    broker.on_next_tick(tick3, book);

    core::MarketTick tick4 = tick1;
    tick4.timestamp_ns = 3000000;
    tick4.last_price = core::to_paise(152.50);
    tick4.bids[0] = {core::to_paise(152.45), 1000, 5};
    tick4.asks[0] = {core::to_paise(152.50), 1000, 5};
    book.on_tick(tick4);
    broker.on_next_tick(tick4, book);

    // Place SELL at tick 4 (fills on tick 5)
    core::Order sell_order = buy_order;
    sell_order.client_order_id = 2;
    sell_order.side = core::Side::SELL;
    sell_order.created_time_ns = 3000000;
    DU_EXPECT(broker.place_order(sell_order));
    DU_EXPECT(!broker.place_order(sell_order)); // Duplicate SELL rejected

    core::MarketTick tick5 = tick1;
    tick5.timestamp_ns = 4000000;
    tick5.last_price = core::to_paise(152.05);
    tick5.bids[0] = {core::to_paise(152.00), 1000, 5};
    tick5.asks[0] = {core::to_paise(152.05), 1000, 5};
    book.on_tick(tick5);
    broker.on_next_tick(tick5, book);

    DU_EXPECT(broker.total_trades_count() == 1);
    const auto& trade = broker.trade_history()[0];
    DU_EXPECT(trade.entry_price > core::to_paise(150.10)); // Adverse BUY slippage applied
    DU_EXPECT(trade.exit_price < core::to_paise(152.00));  // Adverse SELL slippage applied
    DU_EXPECT(trade.max_adverse_excursion == trade.entry_price - core::to_paise(149.60));
    DU_EXPECT(trade.max_favorable_excursion == core::to_paise(152.50) - trade.entry_price);
    DU_EXPECT(trade.gross_pnl_inr > 0.0);
    DU_EXPECT(trade.stt_inr > 0.0);
    DU_EXPECT(trade.exchange_fee_inr > 0.0);
    DU_EXPECT(trade.gst_inr > 0.0);
    DU_EXPECT(trade.stamp_duty_inr > 0.0);
    DU_EXPECT(trade.sebi_turnover_inr > 0.0);
    DU_EXPECT(trade.net_pnl_inr < trade.gross_pnl_inr);
    DU_EXPECT_NEAR(broker.cash_balance_inr(), 10000.0 + trade.net_pnl_inr, 1e-9);

    std::cout << "[PASS] test_paper_matching_excursions_and_statutory_taxes\n";
}

int main() {
    std::cout << "Running PaperBroker tests...\n";
    test_paper_matching_excursions_and_statutory_taxes();
    std::cout << "All PaperBroker tests passed!\n";
    return 0;
}
