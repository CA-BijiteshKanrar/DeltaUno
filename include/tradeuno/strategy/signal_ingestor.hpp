#pragma once

#include "tradeuno/core/types.hpp"
#include "tradeuno/market/order_book.hpp"
#include <string>
#include <optional>
#include <unordered_set>

namespace tradeuno::strategy {

enum class VetoReason : uint8_t {
    NONE = 0,
    NOT_IN_WHITELIST = 1,
    NEGATIVE_ORDER_FLOW = 2,
    OVERCROWDED_EXIT_TRAP = 3,
    NEAR_CIRCUIT_LIMIT = 4,
    INVALID_PARAMETERS = 5
};

[[nodiscard]] constexpr const char* veto_reason_to_string(VetoReason r) noexcept {
    switch (r) {
        case VetoReason::NOT_IN_WHITELIST: return "NOT_IN_WHITELIST";
        case VetoReason::NEGATIVE_ORDER_FLOW: return "NEGATIVE_ORDER_FLOW (Bids < Asks)";
        case VetoReason::OVERCROWDED_EXIT_TRAP: return "OVERCROWDED_EXIT_TRAP (Already Pumped > 2%)";
        case VetoReason::NEAR_CIRCUIT_LIMIT: return "NEAR_CIRCUIT_LIMIT (Liquidity Risk)";
        case VetoReason::INVALID_PARAMETERS: return "INVALID_PARAMETERS";
        default: return "NONE";
    }
}

class SignalIngestor {
public:
    SignalIngestor() {
        whitelist_.insert("TATASTEEL");
        whitelist_.insert("SBIN");
        whitelist_.insert("ITC");
        whitelist_.insert("COALINDIA");
        whitelist_.insert("ZOMATO");
    }

    // Evaluates an external trader alert through the local L2 "BS Detector"
    [[nodiscard]] std::pair<std::optional<core::TradingSignal>, VetoReason>
    validate_external_alert(const std::string& symbol,
                            core::Side side,
                            core::Price suggested_entry,
                            core::Price suggested_sl,
                            const market::OrderBook& book) {
        // 1. Whitelist filter
        if (whitelist_.find(symbol) == whitelist_.end()) {
            return {std::nullopt, VetoReason::NOT_IN_WHITELIST};
        }

        if (side != core::Side::BUY) {
            // For ₹1,000 capital, only cash/intraday longs are permitted safely
            return {std::nullopt, VetoReason::INVALID_PARAMETERS};
        }

        // 2. BS Detector: Verify Order Flow Imbalance (OFI)
        // If external trader says BUY, but local book shows heavy ask dumping, VETO!
        if (book.composite_ofi() < 0.20) {
            return {std::nullopt, VetoReason::NEGATIVE_ORDER_FLOW};
        }

        // 3. Overcrowding & Exit-Trap Filter (Pelster & Hofmann 2018)
        // If current ask price is already > 2.0% above suggested entry, followers are already buying the top
        if (suggested_entry > 0 && book.best_ask() > 0) {
            double price_surge_pct = static_cast<double>(book.best_ask() - suggested_entry) / static_cast<double>(suggested_entry) * 100.0;
            if (price_surge_pct > 2.0) {
                return {std::nullopt, VetoReason::OVERCROWDED_EXIT_TRAP};
            }
        }

        // 4. Circuit Band Trap Check
        if (book.is_near_circuit(1.5)) {
            return {std::nullopt, VetoReason::NEAR_CIRCUIT_LIMIT};
        }

        // Construct validated signal
        core::TradingSignal sig{};
        sig.timestamp_ns = book.last_tick_time_ns();
        sig.token = book.token();
#if defined(_MSC_VER)
        strncpy_s(sig.symbol, symbol.c_str(), sizeof(sig.symbol) - 1);
#else
        std::strncpy(sig.symbol, symbol.c_str(), sizeof(sig.symbol) - 1);
#endif
        sig.side = core::Side::BUY;
        sig.target_entry = book.best_ask();

        // Enforce safe SL: suggested SL or 1% max risk
        core::Price max_allowed_sl = static_cast<core::Price>(sig.target_entry * 0.990);
        sig.stop_loss = (suggested_sl > 0) ? std::max(suggested_sl, max_allowed_sl) : max_allowed_sl;

        sig.take_profit = static_cast<core::Price>(sig.target_entry * 1.018);
        sig.confidence = book.composite_ofi();
        sig.source = core::SignalSource::EXTERNAL_WEBHOOK;
        sig.is_valid = true;

        return {sig, VetoReason::NONE};
    }

private:
    std::unordered_set<std::string> whitelist_;
};

} // namespace tradeuno::strategy
