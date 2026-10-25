#pragma once

#include "tradeuno/core/types.hpp"
#include <vector>
#include <numeric>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace tradeuno::execution {

struct AuditSummary {
    size_t total_trades{0};
    size_t winning_trades{0};
    size_t losing_trades{0};
    size_t breakeven_trades{0};
    double win_rate_pct{0.0};
    double gross_profit_inr{0.0};
    double gross_loss_inr{0.0};
    double total_taxes_and_friction_inr{0.0};
    double net_pnl_inr{0.0};
    double profit_factor{0.0};
    double expectancy_per_trade_inr{0.0};
    double friction_drag_ratio_pct{0.0};
    double avg_mae_paise{0.0};
    double avg_mfe_paise{0.0};
};

class TradeAuditor {
public:
    [[nodiscard]] static AuditSummary evaluate(const std::vector<core::TradeAuditEntry>& trades) {
        AuditSummary s{};
        if (trades.empty()) return s;

        s.total_trades = trades.size();
        double sum_mae = 0.0;
        double sum_mfe = 0.0;

        for (const auto& t : trades) {
            sum_mae += static_cast<double>(t.max_adverse_excursion);
            sum_mfe += static_cast<double>(t.max_favorable_excursion);

            double total_fee = t.stt_inr + t.exchange_fee_inr + t.gst_inr + t.stamp_duty_inr + t.sebi_turnover_inr;
            s.total_taxes_and_friction_inr += total_fee;
            s.net_pnl_inr += t.net_pnl_inr;

            if (t.net_pnl_inr > 0.50) {
                s.winning_trades++;
                s.gross_profit_inr += t.gross_pnl_inr;
            } else if (t.net_pnl_inr < -0.50) {
                s.losing_trades++;
                s.gross_loss_inr += std::abs(t.gross_pnl_inr);
            } else {
                s.breakeven_trades++;
            }
        }

        s.win_rate_pct = (static_cast<double>(s.winning_trades) / static_cast<double>(s.total_trades)) * 100.0;
        s.avg_mae_paise = sum_mae / static_cast<double>(s.total_trades);
        s.avg_mfe_paise = sum_mfe / static_cast<double>(s.total_trades);
        s.expectancy_per_trade_inr = s.net_pnl_inr / static_cast<double>(s.total_trades);

        if (s.gross_loss_inr > 0) {
            s.profit_factor = s.gross_profit_inr / s.gross_loss_inr;
        } else if (s.gross_profit_inr > 0) {
            s.profit_factor = 999.0;
        }

        if (s.gross_profit_inr > 0) {
            s.friction_drag_ratio_pct = (s.total_taxes_and_friction_inr / s.gross_profit_inr) * 100.0;
        }

        return s;
    }

    static void print_report(const AuditSummary& s, std::ostream& out = std::cout) {
        out << "\n============================================================\n";
        out << "              TRADEUNO PERFORMANCE AUDIT REPORT             \n";
        out << "============================================================\n";
        out << std::fixed << std::setprecision(2);
        out << "  Total Completed Trades    : " << s.total_trades << "\n";
        out << "  Winning Trades            : " << s.winning_trades << " (" << s.win_rate_pct << "%)\n";
        out << "  Losing Trades             : " << s.losing_trades << "\n";
        out << "  Break-Even Trades         : " << s.breakeven_trades << "\n";
        out << "------------------------------------------------------------\n";
        out << "  Gross Profit              : +" << s.gross_profit_inr << " INR\n";
        out << "  Gross Loss                : -" << s.gross_loss_inr << " INR\n";
        out << "  Total Statutory Friction  : -" << s.total_taxes_and_friction_inr << " INR (STT/GST/Turnover)\n";
        out << "  Net PnL                   : " << (s.net_pnl_inr >= 0 ? "+" : "") << s.net_pnl_inr << " INR\n";
        out << "  Profit Factor             : " << s.profit_factor << "\n";
        out << "  Expectancy / Trade        : " << (s.expectancy_per_trade_inr >= 0 ? "+" : "") << s.expectancy_per_trade_inr << " INR\n";
        out << "  Friction Drag Ratio       : " << s.friction_drag_ratio_pct << "% of gross profit\n";
        out << "------------------------------------------------------------\n";
        out << "  Avg MAE (Adverse Move)    : " << (s.avg_mae_paise / 100.0) << " INR\n";
        out << "  Avg MFE (Favorable Move)  : " << (s.avg_mfe_paise / 100.0) << " INR\n";
        out << "============================================================\n\n";
    }
};

} // namespace tradeuno::execution
