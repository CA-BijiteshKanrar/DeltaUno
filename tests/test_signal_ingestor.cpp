#include "deltauno/strategy/signal_ingestor.hpp"
#include "test_macros.hpp"

#include <iostream>

using namespace deltauno;

namespace {

market::OrderBook make_bullish_book(core::Price ask_paise = core::to_paise(150.25),
                                    core::Price upper_c = core::to_paise(165.00)) {
    market::OrderBook book(349121, "TATASTEEL");

    core::MarketTick t1{};
    t1.token = 349121;
    core::copy_fixed_str(t1.symbol, "TATASTEEL");
    t1.timestamp_ns = 1000;
    t1.last_price = ask_paise - 5;
    t1.upper_circuit = upper_c;
    t1.lower_circuit = core::to_paise(135.00);
    t1.bids[0] = {ask_paise - 10, 800, 4};
    t1.asks[0] = {ask_paise, 900, 4};
    book.on_tick(t1);

    core::MarketTick t2 = t1;
    t2.timestamp_ns = 2000;
    t2.bids[0] = {ask_paise - 5, 3500, 15};
    t2.asks[0] = {ask_paise, 200, 2};
    book.on_tick(t2);

    return book;
}

} // namespace

void test_webhook_json_parsing_and_valid_signal() {
    strategy::SignalIngestor ingestor;
    const auto book = make_bullish_book();

    const std::string payload =
        R"({"symbol":"TATASTEEL","token":349121,"side":"BUY","entry":150.00,"sl":149.20,"timestamp_ns":2000})";
    const auto alert_opt = strategy::SignalIngestor::parse_webhook_json(payload);
    DU_EXPECT(alert_opt.has_value());
    DU_EXPECT(std::string(alert_opt->symbol) == "TATASTEEL");
    DU_EXPECT(alert_opt->suggested_entry == core::to_paise(150.00));

    const auto [sig, veto] = ingestor.validate_external_alert(*alert_opt, book, 2.2);
    DU_EXPECT(veto == strategy::VetoReason::NONE);
    DU_EXPECT(sig.has_value());
    DU_EXPECT(sig->source == core::SignalSource::EXTERNAL_WEBHOOK);
    // Tighter suggested SL (149.20) must be preserved over 1% fallback (148.70)
    DU_EXPECT(sig->stop_loss == core::to_paise(149.20));

    std::cout << "[PASS] test_webhook_json_parsing_and_valid_signal\n";
}

void test_bs_detector_vetoes() {
    strategy::SignalIngestor ingestor;
    const auto bullish_book = make_bullish_book();

    // 1. Whitelist veto (penny pump stock)
    {
        const auto [sig, veto] = ingestor.validate_external_alert(
            "PENNYPUMP", core::Side::BUY, core::to_paise(12.00), core::to_paise(11.50), bullish_book, 1.5);
        DU_EXPECT(!sig.has_value());
        DU_EXPECT(veto == strategy::VetoReason::NOT_IN_WHITELIST);
    }

    // 2. Short-sell veto on micro-account
    {
        const auto [sig, veto] = ingestor.validate_external_alert(
            "TATASTEEL", core::Side::SELL, core::to_paise(150.00), core::to_paise(151.50), bullish_book, 1.5);
        DU_EXPECT(!sig.has_value());
        DU_EXPECT(veto == strategy::VetoReason::INVALID_PARAMETERS);
    }

    // 3. Negative Order Flow veto (external alert says BUY while asks dump heavily)
    {
        market::OrderBook bearish_book = bullish_book;
        core::MarketTick dump{};
        dump.token = 349121;
        dump.timestamp_ns = 3000;
        dump.last_price = core::to_paise(150.10);
        dump.upper_circuit = core::to_paise(165.00);
        dump.lower_circuit = core::to_paise(135.00);
        dump.bids[0] = {core::to_paise(150.05), 150, 1};
        dump.asks[0] = {core::to_paise(150.10), 6000, 25};
        bearish_book.on_tick(dump);

        const auto [sig, veto] = ingestor.validate_external_alert(
            "TATASTEEL", core::Side::BUY, core::to_paise(150.00), core::to_paise(148.80), bearish_book, 1.5);
        DU_EXPECT(!sig.has_value());
        DU_EXPECT(veto == strategy::VetoReason::NEGATIVE_ORDER_FLOW);
    }

    // 4. Overcrowded > 2.0% price pump veto
    {
        const auto pumped_book = make_bullish_book(core::to_paise(154.00)); // +2.67% above 150.00 alert
        const auto [sig, veto] = ingestor.validate_external_alert(
            "TATASTEEL", core::Side::BUY, core::to_paise(150.00), core::to_paise(148.80), pumped_book, 2.0);
        DU_EXPECT(!sig.has_value());
        DU_EXPECT(veto == strategy::VetoReason::OVERCROWDED_EXIT_TRAP);
    }

    // 5. Overcrowded > 4.0x volume climax veto
    {
        const auto [sig, veto] = ingestor.validate_external_alert(
            "TATASTEEL", core::Side::BUY, core::to_paise(150.00), core::to_paise(148.80), bullish_book, 4.5);
        DU_EXPECT(!sig.has_value());
        DU_EXPECT(veto == strategy::VetoReason::OVERCROWDED_EXIT_TRAP);
    }

    // 6. Near upper circuit trap veto
    {
        const auto circuit_book = make_bullish_book(core::to_paise(164.00), core::to_paise(165.00));
        const auto [sig, veto] = ingestor.validate_external_alert(
            "TATASTEEL", core::Side::BUY, core::to_paise(163.50), core::to_paise(162.00), circuit_book, 1.8);
        DU_EXPECT(!sig.has_value());
        DU_EXPECT(veto == strategy::VetoReason::NEAR_CIRCUIT_LIMIT);
    }

    std::cout << "[PASS] test_bs_detector_vetoes\n";
}

int main() {
    std::cout << "Running SignalIngestor (BS Detector) tests...\n";
    test_webhook_json_parsing_and_valid_signal();
    test_bs_detector_vetoes();
    std::cout << "All SignalIngestor tests passed!\n";
    return 0;
}
