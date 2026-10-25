#include "tradeuno/core/state_journal.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>

using namespace tradeuno;

void test_crash_recovery_journal() {
    std::string test_file = "test_state.json";
    if (std::filesystem::exists(test_file)) {
        std::filesystem::remove(test_file);
    }

    core::StateJournal journal(test_file);

    // Initial state: empty
    assert(!journal.recover_position().has_value());

    // Save position before simulated crash
    core::Position pos{};
    pos.token = 349121;
#if defined(_MSC_VER)
    strncpy_s(pos.symbol, "TATASTEEL", sizeof(pos.symbol) - 1);
#else
    std::strncpy(pos.symbol, "TATASTEEL", sizeof(pos.symbol) - 1);
#endif
    pos.side = core::Side::BUY;
    pos.quantity = 3;
    pos.avg_entry_price = core::to_paise(150.25);
    pos.current_stop_loss = core::to_paise(148.75);
    pos.take_profit = core::to_paise(153.00);
    pos.open_time_ns = 1730002200000000000ULL;

    assert(journal.save_position(pos));

    // Simulate engine crash and reboot by instantiating new journal instance
    core::StateJournal recovered_journal(test_file);
    auto rec_pos = recovered_journal.recover_position();
    assert(rec_pos.has_value());
    assert(rec_pos->token == 349121);
    assert(std::string(rec_pos->symbol) == "TATASTEEL");
    assert(rec_pos->quantity == 3);
    assert(rec_pos->avg_entry_price == core::to_paise(150.25));

    // Clear position on trade exit
    assert(recovered_journal.clear_position());
    assert(!recovered_journal.recover_position().has_value());

    if (std::filesystem::exists(test_file)) {
        std::filesystem::remove(test_file);
    }

    std::cout << "[PASS] test_crash_recovery_journal\n";
}

int main() {
    std::cout << "Running StateJournal tests...\n";
    test_crash_recovery_journal();
    std::cout << "All StateJournal tests passed!\n";
    return 0;
}
