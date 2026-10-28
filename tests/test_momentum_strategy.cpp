#include "deltauno/strategy/momentum_breakout.hpp"
#include "deltauno/market/order_book.hpp"
#include "test_macros.hpp"

#include <iostream>

using namespace deltauno;

void test_per_symbol_vwap_and_volume_isolation() {
    strategy::MomentumBreakoutStrategy strat(2.5, 0.55);
    market::OrderBook tata_book(349121, "TATASTEEL");
    market::OrderBook sbin_book(779521, "SBIN");

    // Interleave 15 ticks of TATASTEEL (LTP=₹150.00, vol delta=100) and SBIN (LTP=₹800.00, vol delta=5000)
    uint64_t tata_vol = 100000;
    uint64_t sbin_vol = 500000;

    for (int i = 0; i < 15; ++i) {
        // TATASTEEL Tick
        core::MarketTick t_tata{};
        t_tata.token = 349121;
        core::copy_fixed_str(t_tata.symbol, "TATASTEEL");
        t_tata.timestamp_ns = 1000000ULL + static_cast<uint64_t>(i) * 2000000ULL;
        t_tata.last_price = core::to_paise(150.00);
        t_tata.last_quantity = 100;
        tata_vol += 100;
        t_tata.total_volume = tata_vol;
        t_tata.bids[0] = {core::to_paise(149.95), 1000, 5};
        t_tata.asks[0] = {core::to_paise(150.05), 1000, 5};
        tata_book.on_tick(t_tata);
        strat.on_tick(t_tata, tata_book);

        // SBIN Tick
        core::MarketTick t_sbin{};
        t_sbin.token = 779521;
        core::copy_fixed_str(t_sbin.symbol, "SBIN");
        t_sbin.timestamp_ns = 2000000ULL + static_cast<uint64_t>(i) * 2000000ULL;
        t_sbin.last_price = core::to_paise(800.00);
        t_sbin.last_quantity = 5000;
        sbin_vol += 5000;
        t_sbin.total_volume = sbin_vol;
        t_sbin.bids[0] = {core::to_paise(799.80), 2000, 10};
        t_sbin.asks[0] = {core::to_paise(800.20), 2000, 10};
        sbin_book.on_tick(t_sbin);
        strat.on_tick(t_sbin, sbin_book);
    }

    // Verify completely isolated VWAPs:
    // TATASTEEL VWAP must be exactly ₹150.00 (15000 paise), NOT contaminated by SBIN ₹800
    DU_EXPECT(strat.get_vwap(349121) == core::to_paise(150.00));
    DU_EXPECT(strat.get_vwap("TATASTEEL") == core::to_paise(150.00));

    // SBIN VWAP must be exactly ₹800.00 (80000 paise), NOT diluted by TATASTEEL ₹150
    DU_EXPECT(strat.get_vwap(779521) == core::to_paise(800.00));
    DU_EXPECT(strat.get_vwap("SBIN") == core::to_paise(800.00));

    // Verify baseline volume multiple is ~1.0 for both
    DU_EXPECT_NEAR(strat.latest_volume_multiple(349121), 1.0, 0.05);
    DU_EXPECT_NEAR(strat.latest_volume_multiple(779521), 1.0, 0.05);

    // Now inject volume surge ONLY into TATASTEEL (350 shares vs 100 baseline -> 3.5x multiple)
    core::MarketTick surge_tata{};
    surge_tata.token = 349121;
    core::copy_fixed_str(surge_tata.symbol, "TATASTEEL");
    surge_tata.timestamp_ns = 1000000ULL + 15ULL * 2000000ULL;
    surge_tata.last_price = core::to_paise(150.50);
    surge_tata.last_quantity = 350;
    tata_vol += 350;
    surge_tata.total_volume = tata_vol;
    surge_tata.bids[0] = {core::to_paise(150.45), 5000, 20}; // strong bid support
    surge_tata.asks[0] = {core::to_paise(150.55), 500, 2};
    tata_book.on_tick(surge_tata);
    strat.on_tick(surge_tata, tata_book);

    // TATASTEEL volume multiple must exceed 2.5x threshold
    DU_EXPECT(strat.latest_volume_multiple(349121) >= 2.5);

    // SBIN volume multiple must remain completely untouched (~1.0)
    DU_EXPECT_NEAR(strat.latest_volume_multiple(779521), 1.0, 0.05);

    std::cout << "[PASS] test_per_symbol_vwap_and_volume_isolation\n";
}

void test_daily_session_rollover_reset() {
    strategy::MomentumBreakoutStrategy strat(2.5, 0.55);
    market::OrderBook book(349121, "TATASTEEL");

    // Populate state on Day 1
    core::MarketTick t1{};
    t1.token = 349121;
    core::copy_fixed_str(t1.symbol, "TATASTEEL");
    t1.last_price = core::to_paise(150.00);
    t1.last_quantity = 500;
    t1.total_volume = 100000;
    book.on_tick(t1);
    strat.on_tick(t1, book);

    core::MarketTick t2 = t1;
    t2.last_quantity = 500;
    t2.total_volume = 100500;
    book.on_tick(t2);
    strat.on_tick(t2, book);

    DU_EXPECT(strat.get_vwap(349121) == core::to_paise(150.00));

    // Daily rollover event at 09:15 IST
    strat.reset_daily_session();

    // VWAP and volume multiple must be reset to 0
    DU_EXPECT(strat.get_vwap(349121) == 0);
    DU_EXPECT(strat.latest_volume_multiple(349121) == 0.0);

    // First tick of Day 2 establishes fresh baseline
    core::MarketTick day2_t1{};
    day2_t1.token = 349121;
    day2_t1.symbol[0] = '\0';
    day2_t1.last_price = core::to_paise(155.00);
    day2_t1.last_quantity = 200;
    day2_t1.total_volume = 200;
    book.on_tick(day2_t1);
    strat.on_tick(day2_t1, book);

    // Day 2 VWAP starts cleanly at ₹155.00
    DU_EXPECT(strat.get_vwap(349121) == core::to_paise(155.00));
    DU_EXPECT(strat.latest_volume_multiple(349121) == 0.0); // only 1 tick in window

    std::cout << "[PASS] test_daily_session_rollover_reset\n";
}

void test_macro_regime_and_vwap_shield() {
    strategy::MomentumBreakoutStrategy strat(2.5, 0.55);
    market::OrderBook book(349121, "TATASTEEL");

    // 1. Send NIFTY open tick at 22,700
    core::MarketTick nifty_t1{};
    nifty_t1.token = 999999;
    core::copy_fixed_str(nifty_t1.symbol, "NIFTY");
    nifty_t1.last_price = core::to_paise(22700.0);
    strat.on_tick(nifty_t1, book);
    DU_EXPECT(!strat.is_macro_bearish());
    DU_EXPECT_NEAR(strat.nifty_return_pct(), 0.0, 0.01);

    // 2. NIFTY drops to 22,550 (-0.66% sell-off)
    core::MarketTick nifty_t2 = nifty_t1;
    nifty_t2.last_price = core::to_paise(22550.0);
    strat.on_tick(nifty_t2, book);
    DU_EXPECT(strat.is_macro_bearish());
    DU_EXPECT(strat.nifty_return_pct() < -0.35);

    // 3. Even with huge volume surge and OFI on TATASTEEL, signal must be vetoed by macro gate
    for (int i = 0; i < 20; ++i) {
        core::MarketTick t{};
        t.token = 349121;
        core::copy_fixed_str(t.symbol, "TATASTEEL");
        t.last_price = core::to_paise(150.0);
        t.last_quantity = 1000;
        t.total_volume = 100000 + (i + 1) * 1000;
        t.bids[0] = {core::to_paise(149.95), 5000, 10};
        t.asks[0] = {core::to_paise(150.05), 100, 1};
        book.on_tick(t);
        strat.on_tick(t, book);
    }
    const auto sig = strat.generate_signal(book);
    DU_EXPECT(!sig.has_value()); // Blocked by macro regime!

    // 4. Recover NIFTY back to flat (22,710)
    core::MarketTick nifty_t3 = nifty_t1;
    nifty_t3.last_price = core::to_paise(22710.0);
    strat.on_tick(nifty_t3, book);
    DU_EXPECT(!strat.is_macro_bearish());

    std::cout << "[PASS] test_macro_regime_and_vwap_shield\n";
}

int main() {
    std::cout << "Running MomentumBreakoutStrategy tests...\n";
    test_per_symbol_vwap_and_volume_isolation();
    test_daily_session_rollover_reset();
    test_macro_regime_and_vwap_shield();
    std::cout << "All MomentumBreakoutStrategy tests passed!\n";
    return 0;
}
