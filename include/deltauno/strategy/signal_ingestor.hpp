#pragma once

#include "deltauno/core/config_loader.hpp"
#include "deltauno/core/types.hpp"
#include "deltauno/market/order_book.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace deltauno::strategy {

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
        case VetoReason::OVERCROWDED_EXIT_TRAP: return "OVERCROWDED_EXIT_TRAP (Already Pumped > 2% or Vol > 4x)";
        case VetoReason::NEAR_CIRCUIT_LIMIT: return "NEAR_CIRCUIT_LIMIT (Liquidity Risk)";
        case VetoReason::INVALID_PARAMETERS: return "INVALID_PARAMETERS";
        default: return "NONE";
    }
}

class SignalIngestor {
public:
    explicit SignalIngestor(std::unordered_set<std::string> whitelist = {},
                            double max_price_surge_pct = 2.0,
                            double max_volume_burst_multiple = 4.0,
                            double min_ofi = 0.20,
                            double circuit_buffer_pct = 1.5,
                            core::Price tick_size_paise = 5)
        : whitelist_(std::move(whitelist)),
          max_price_surge_pct_(max_price_surge_pct),
          max_volume_burst_multiple_(max_volume_burst_multiple),
          min_ofi_(min_ofi),
          circuit_buffer_pct_(circuit_buffer_pct),
          tick_size_paise_(tick_size_paise) {
        if (whitelist_.empty()) {
            whitelist_ = {"TATASTEEL", "SBIN", "ITC", "COALINDIA", "ZOMATO"};
        }
    }

    // Parses external JSON webhook payload:
    // {"symbol":"TATASTEEL","token":349121,"side":"BUY","entry":150.20,"sl":148.80,"timestamp_ns":1793765712000000000}
    [[nodiscard]] static std::optional<core::ExternalAlert> parse_webhook_json(std::string_view json) {
        const auto sym = core::ConfigLoader::extract_string(json, "symbol");
        if (!sym || sym->empty()) return std::nullopt;

        core::ExternalAlert alert{};
        core::copy_fixed_str(alert.symbol, *sym);

        if (const auto tok = core::ConfigLoader::extract_number(json, "token")) {
            alert.token = static_cast<uint32_t>(*tok);
        }
        if (const auto ts = core::ConfigLoader::extract_number(json, "timestamp_ns")) {
            alert.timestamp_ns = static_cast<uint64_t>(*ts);
        }
        if (const auto side = core::ConfigLoader::extract_string(json, "side")) {
            alert.side = (*side == "SELL") ? core::Side::SELL : core::Side::BUY;
        }
        if (const auto entry = core::ConfigLoader::extract_number(json, "entry")) {
            alert.suggested_entry = core::to_paise(*entry);
        }
        if (const auto sl = core::ConfigLoader::extract_number(json, "sl")) {
            alert.suggested_sl = core::to_paise(*sl);
        }
        return alert;
    }

    // Evaluates an external trader alert through the local L2 "BS Detector" & Overcrowding Guard
    [[nodiscard]] std::pair<std::optional<core::TradingSignal>, VetoReason>
    validate_external_alert(const std::string& symbol,
                            core::Side side,
                            core::Price suggested_entry,
                            core::Price suggested_sl,
                            const market::OrderBook& book,
                            double current_volume_multiple = 0.0) const {
        // 1. Whitelist filter
        if (whitelist_.find(symbol) == whitelist_.end()) {
            return {std::nullopt, VetoReason::NOT_IN_WHITELIST};
        }

        if (side != core::Side::BUY || book.best_ask() <= 0) {
            // For ₹10,000 pilot capital, only cash/MIS longs with live ask depth are permitted
            return {std::nullopt, VetoReason::INVALID_PARAMETERS};
        }

        // 2. BS Detector: Verify local L2 Order Flow Imbalance (OFI) & Stoikov Microprice Queue Support
        if (book.composite_ofi() < min_ofi_ || book.weighted_mid_price() < book.mid_price()) {
            return {std::nullopt, VetoReason::NEGATIVE_ORDER_FLOW};
        }

        // 2b. Minimum Viable Stop-Loss Cushion (>= 0.25% below ask to prevent 1-2 tick noise stop-out traps)
        if (suggested_sl > 0 && suggested_sl < book.best_ask()) {
            const double sl_dist_pct =
                static_cast<double>(book.best_ask() - suggested_sl) / static_cast<double>(book.best_ask()) * 100.0;
            if (sl_dist_pct < 0.25) {
                return {std::nullopt, VetoReason::INVALID_PARAMETERS};
            }
        }

        // 3. Overcrowding & Exit-Trap Filter (Pelster & Hofmann 2018)
        // Reject if ask already surged > 2.0% past alert entry OR if volume already exploded > 4.0x
        if (suggested_entry > 0) {
            const double price_surge_pct =
                static_cast<double>(book.best_ask() - suggested_entry) / static_cast<double>(suggested_entry) * 100.0;
            if (price_surge_pct > max_price_surge_pct_) {
                return {std::nullopt, VetoReason::OVERCROWDED_EXIT_TRAP};
            }
        }
        if (max_volume_burst_multiple_ > 0.0 && current_volume_multiple > max_volume_burst_multiple_) {
            return {std::nullopt, VetoReason::OVERCROWDED_EXIT_TRAP};
        }

        // 4. Circuit Band Trap Check
        if (book.is_near_circuit(circuit_buffer_pct_)) {
            return {std::nullopt, VetoReason::NEAR_CIRCUIT_LIMIT};
        }

        // Construct validated signal
        core::TradingSignal sig{};
        sig.timestamp_ns = book.last_tick_time_ns();
        sig.token = book.token();
        core::copy_fixed_str(sig.symbol, symbol);
        sig.side = core::Side::BUY;
        sig.target_entry = book.best_ask();

        // Enforce safe SL: tighter of suggested SL or 1% max per-share distance, aligned to NSE tick size
        const core::Price max_allowed_sl = core::floor_to_tick(
            static_cast<core::Price>(static_cast<double>(sig.target_entry) * 0.990), tick_size_paise_);
        const core::Price raw_sl = (suggested_sl > 0 && suggested_sl < sig.target_entry)
                                       ? std::max(core::floor_to_tick(suggested_sl, tick_size_paise_), max_allowed_sl)
                                       : max_allowed_sl;
        sig.stop_loss = raw_sl;
        sig.take_profit = core::ceil_to_tick(
            static_cast<core::Price>(static_cast<double>(sig.target_entry) * 1.018), tick_size_paise_);

        sig.confidence = std::clamp(book.composite_ofi(), 0.0, 1.0);
        sig.source = core::SignalSource::EXTERNAL_WEBHOOK;
        sig.is_valid = true;

        return {sig, VetoReason::NONE};
    }

    [[nodiscard]] std::pair<std::optional<core::TradingSignal>, VetoReason>
    validate_external_alert(const core::ExternalAlert& alert,
                            const market::OrderBook& book,
                            double current_volume_multiple = 0.0) const {
        return validate_external_alert(
            std::string(alert.symbol),
            alert.side,
            alert.suggested_entry,
            alert.suggested_sl,
            book,
            current_volume_multiple);
    }

private:
    std::unordered_set<std::string> whitelist_;
    double max_price_surge_pct_{2.0};
    double max_volume_burst_multiple_{4.0};
    double min_ofi_{0.20};
    double circuit_buffer_pct_{1.5};
    core::Price tick_size_paise_{5};
};

} // namespace deltauno::strategy
