#include "deltauno/core/state_journal.hpp"
#include "deltauno/execution/paper_broker.hpp"
#include "deltauno/market/watchdog.hpp"
#include "test_macros.hpp"

#include <filesystem>
#include <iostream>

using namespace deltauno;

void test_crash_recovery_journal_and_broker_restore() {
    const std::string test_file = "test_state_deltauno.json";
    std::filesystem::remove(test_file);

    core::StateJournal journal(test_file);
    DU_EXPECT(!journal.recover_position().has_value());

    core::Position pos{};
    pos.token = 349121;
    core::copy_fixed_str(pos.symbol, "TATASTEEL");
    pos.side = core::Side::BUY;
    pos.quantity = 3;
    pos.avg_entry_price = core::to_paise(150.25);
    pos.current_stop_loss = core::to_paise(148.75);
    pos.take_profit = core::to_paise(153.00);
    pos.peak_favorable_price = core::to_paise(151.10);
    pos.worst_adverse_price = core::to_paise(150.00);
    pos.open_time_ns = 1730002200000000000ULL;

    DU_EXPECT(journal.save_position(pos));
    DU_EXPECT(!std::filesystem::exists(test_file + ".tmp")); // Atomic rename completed

    // Reboot & restore into PaperBroker, then verify post-recovery SELL executes cleanly
    core::StateJournal recovered_journal(test_file);
    const auto rec_pos = recovered_journal.recover_position();
    DU_EXPECT(rec_pos.has_value());
    DU_EXPECT(rec_pos->token == 349121);
    DU_EXPECT(std::string(rec_pos->symbol) == "TATASTEEL");
    DU_EXPECT(rec_pos->quantity == 3);
    DU_EXPECT(rec_pos->avg_entry_price == core::to_paise(150.25));

    execution::PaperBroker broker(10000.0);
    broker.restore_open_position(*rec_pos);
    DU_EXPECT(broker.has_open_long());

    core::Order exit_sell{};
    exit_sell.client_order_id = 99;
    exit_sell.token = 349121;
    core::copy_fixed_str(exit_sell.symbol, "TATASTEEL");
    exit_sell.side = core::Side::SELL;
    exit_sell.quantity = 3;
    exit_sell.created_time_ns = rec_pos->open_time_ns + 1000;
    DU_EXPECT(broker.place_order(exit_sell));

    market::OrderBook book(349121, "TATASTEEL");
    core::MarketTick next_tick{};
    next_tick.token = 349121;
    next_tick.timestamp_ns = rec_pos->open_time_ns + 2000;
    next_tick.last_price = core::to_paise(152.00);
    next_tick.bids[0] = {core::to_paise(152.00), 1000, 5};
    next_tick.asks[0] = {core::to_paise(152.05), 1000, 5};
    book.on_tick(next_tick);
    broker.on_next_tick(next_tick, book);

    DU_EXPECT(!broker.has_open_long());
    DU_EXPECT(broker.total_trades_count() == 1);
    // Preserved pre-crash MAE (150.25 - 150.00 = 25 paise)
    DU_EXPECT(broker.trade_history()[0].max_adverse_excursion == 25);

    DU_EXPECT(recovered_journal.clear_position());
    DU_EXPECT(!recovered_journal.recover_position().has_value());
    std::filesystem::remove(test_file);

    std::cout << "[PASS] test_crash_recovery_journal_and_broker_restore\n";
}

void test_watchdog_tick_gap_and_clock_safety() {
    market::Watchdog watchdog(5);
    int alerts = 0;
    watchdog.register_alert_callback([&](const std::string&) { ++alerts; });

    const uint64_t t0 = 1793765700000000000ULL;
    watchdog.on_tick(t0);
    // 10s gap while flat -> must NOT alert
    watchdog.on_tick(t0 + 10'000'000'000ULL);
    DU_EXPECT(alerts == 0);

    // Open position + 2s gap -> normal, no alert
    watchdog.set_has_open_position(true);
    watchdog.on_tick(t0 + 12'000'000'000ULL);
    DU_EXPECT(alerts == 0);

    // Open position + 7s gap (> 5s timeout) -> deterministic alert!
    watchdog.on_tick(t0 + 19'000'000'000ULL);
    DU_EXPECT(alerts == 1);
    DU_EXPECT(watchdog.alert_count() == 1);

    std::cout << "[PASS] test_watchdog_tick_gap_and_clock_safety\n";
}

int main() {
    std::cout << "Running StateJournal & Watchdog tests...\n";
    test_crash_recovery_journal_and_broker_restore();
    test_watchdog_tick_gap_and_clock_safety();
    std::cout << "All StateJournal & Watchdog tests passed!\n";
    return 0;
}
