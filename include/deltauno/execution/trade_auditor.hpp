#pragma once

#include "deltauno/core/types.hpp"
#include "deltauno/risk/risk_manager.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace deltauno::execution {

struct DailySessionAudit {
    std::string date_ist;
    int64_t ist_day_index{0};
    size_t total_trades{0};
    size_t winning_trades{0};
    size_t losing_trades{0};
    size_t breakeven_trades{0};
    double win_rate_pct{0.0};
    double gross_profit_inr{0.0};
    double gross_loss_inr{0.0};
    double gross_pnl_inr{0.0};
    double net_pnl_inr{0.0};
    double starting_equity_inr{0.0};
    double ending_equity_inr{0.0};
    double max_intraday_drawdown_inr{0.0};
    bool kill_switch_triggered{false};
    double stt_inr{0.0};
    double exchange_fee_inr{0.0};
    double gst_inr{0.0};
    double stamp_duty_inr{0.0};
    double sebi_turnover_inr{0.0};
    double total_friction_inr{0.0};
    double turnover_inr{0.0};
};

struct EquityCurvePoint {
    std::string date_ist;
    int64_t ist_day_index{0};
    size_t day_number{0};
    double starting_equity_inr{10000.0};
    double closing_equity_inr{10000.0};
    double daily_net_pnl_inr{0.0};
    double daily_return_pct{0.0};
    double cumulative_net_pnl_inr{0.0};
    double cumulative_return_pct{0.0};
    double high_water_mark_inr{10000.0};
    double drawdown_from_hwm_inr{0.0};
    double drawdown_from_hwm_pct{0.0};
    size_t trade_count{0};
};

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

    // Milestone 3 Extended Statutory Friction Sums
    double total_stt_inr{0.0};
    double total_exchange_fee_inr{0.0};
    double total_gst_inr{0.0};
    double total_stamp_duty_inr{0.0};
    double total_sebi_turnover_inr{0.0};
    double total_turnover_inr{0.0};

    // Milestone 3 Multi-Day Capital & Drawdown Metrics
    double initial_capital_inr{10000.0};
    double ending_capital_inr{10000.0};
    double net_return_pct{0.0};
    double peak_capital_inr{10000.0};
    double max_portfolio_drawdown_inr{0.0};
    double max_portfolio_drawdown_pct{0.0};
    size_t total_sessions{0};
    size_t total_kill_switch_triggers{0};
    size_t profitable_days{0};
    size_t losing_days{0};
    size_t scratch_days{0};
    double daily_win_rate_pct{0.0};
};

struct MultiDayAuditReport {
    AuditSummary aggregate;
    std::vector<DailySessionAudit> daily_sessions;
    std::vector<risk::DailyKillSwitchAudit> kill_switch_history;
    std::vector<EquityCurvePoint> equity_curve;
};

class TradeAuditor {
public:
    explicit TradeAuditor(double initial_capital_inr = 10000.0)
        : initial_capital_inr_(initial_capital_inr) {}

    [[nodiscard]] double initial_capital_inr() const noexcept {
        return initial_capital_inr_;
    }

    void set_initial_capital_inr(double cap) noexcept {
        initial_capital_inr_ = cap;
    }

    void on_session_start(int64_t ist_day_index, const std::string& date_ist) {
        if (registered_sessions_.empty() || registered_sessions_.back().first != ist_day_index) {
            registered_sessions_.emplace_back(ist_day_index, date_ist);
        }
    }

    [[nodiscard]] const std::vector<std::pair<int64_t, std::string>>& registered_sessions() const noexcept {
        return registered_sessions_;
    }

    [[nodiscard]] static int64_t get_ist_day_index(uint64_t time_ns) noexcept {
        constexpr uint64_t kIstOffsetSec = 19800ULL;
        constexpr uint64_t kSecPerDay = 86400ULL;
        return static_cast<int64_t>((time_ns / 1'000'000'000ULL + kIstOffsetSec) / kSecPerDay);
    }

    [[nodiscard]] static std::string format_ist_date(uint64_t time_ns) {
        if (time_ns == 0) return "1970-01-01";
        const time_t sec = static_cast<time_t>((time_ns / 1'000'000'000ULL) + 19800ULL);
        std::tm tm_val{};
#if defined(_WIN32) || defined(_WIN64)
        gmtime_s(&tm_val, &sec);
#else
        gmtime_r(&sec, &tm_val);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_val);
        return std::string(buf);
    }

    [[nodiscard]] static std::string format_ist_date_from_day_index(int64_t day_index) {
        if (day_index < 0) return "1970-01-01";
        const time_t sec = static_cast<time_t>(day_index * 86400ULL);
        std::tm tm_val{};
#if defined(_WIN32) || defined(_WIN64)
        gmtime_s(&tm_val, &sec);
#else
        gmtime_r(&sec, &tm_val);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_val);
        return std::string(buf);
    }

    // 100% Backward-compatible evaluate() for single-day / standalone unit tests
    [[nodiscard]] static AuditSummary evaluate(const std::vector<core::TradeAuditEntry>& trades) {
        AuditSummary s{};
        if (trades.empty()) return s;

        s.total_trades = trades.size();
        double sum_mae = 0.0;
        double sum_mfe = 0.0;
        constexpr double kBreakevenBandInr = 0.05;

        for (const auto& t : trades) {
            sum_mae += static_cast<double>(t.max_adverse_excursion);
            sum_mfe += static_cast<double>(t.max_favorable_excursion);

            const double total_fee = t.total_friction_inr();
            s.total_taxes_and_friction_inr += total_fee;
            s.total_stt_inr += t.stt_inr;
            s.total_exchange_fee_inr += t.exchange_fee_inr;
            s.total_gst_inr += t.gst_inr;
            s.total_stamp_duty_inr += t.stamp_duty_inr;
            s.total_sebi_turnover_inr += t.sebi_turnover_inr;
            s.total_turnover_inr += t.turnover_inr();

            s.net_pnl_inr += t.net_pnl_inr;

            if (t.gross_pnl_inr > 0.0) {
                s.gross_profit_inr += t.gross_pnl_inr;
            } else if (t.gross_pnl_inr < 0.0) {
                s.gross_loss_inr += std::abs(t.gross_pnl_inr);
            }

            if (t.net_pnl_inr > kBreakevenBandInr) {
                s.winning_trades++;
            } else if (t.net_pnl_inr < -kBreakevenBandInr) {
                s.losing_trades++;
            } else {
                s.breakeven_trades++;
            }
        }

        s.win_rate_pct =
            (static_cast<double>(s.winning_trades) / static_cast<double>(s.total_trades)) * 100.0;
        s.avg_mae_paise = sum_mae / static_cast<double>(s.total_trades);
        s.avg_mfe_paise = sum_mfe / static_cast<double>(s.total_trades);
        s.expectancy_per_trade_inr = s.net_pnl_inr / static_cast<double>(s.total_trades);

        if (s.gross_loss_inr > 0.0) {
            s.profit_factor = s.gross_profit_inr / s.gross_loss_inr;
        } else if (s.gross_profit_inr > 0.0) {
            s.profit_factor = 999.0;
        }

        if (s.gross_profit_inr > 0.0) {
            s.friction_drag_ratio_pct = (s.total_taxes_and_friction_inr / s.gross_profit_inr) * 100.0;
        }

        return s;
    }

    // Comprehensive Multi-Day Performance Evaluation
    [[nodiscard]] static MultiDayAuditReport evaluate_multi_day(
        const std::vector<core::TradeAuditEntry>& trades,
        double initial_capital_inr = 10000.0,
        const std::vector<std::pair<int64_t, std::string>>& registered_sessions = {},
        const std::vector<risk::DailyKillSwitchAudit>& kill_switch_history = {},
        double kill_switch_threshold_inr = 500.0) {

        MultiDayAuditReport report{};
        report.aggregate = evaluate(trades);
        report.aggregate.initial_capital_inr = initial_capital_inr;

        // Index risk manager kill-switch audits by day index
        std::map<int64_t, risk::DailyKillSwitchAudit> ks_by_day;
        for (const auto& ks : kill_switch_history) {
            ks_by_day[ks.ist_day_index] = ks;
        }

        // Group trades by IST epoch day index
        std::map<int64_t, std::vector<core::TradeAuditEntry>> session_trades;
        for (const auto& t : trades) {
            const uint64_t sample_time = (t.entry_time_ns > 0) ? t.entry_time_ns : t.exit_time_ns;
            const int64_t day_idx = get_ist_day_index(sample_time);
            session_trades[day_idx].push_back(t);
        }

        // Build master list of unique sessions in chronological order
        std::map<int64_t, std::string> all_sessions;
        for (const auto& ks : kill_switch_history) {
            all_sessions[ks.ist_day_index] = ks.date_ist;
        }
        for (const auto& [day_idx, date_str] : registered_sessions) {
            if (all_sessions.find(day_idx) == all_sessions.end()) {
                all_sessions[day_idx] = date_str;
            }
        }
        for (const auto& [day_idx, trs] : session_trades) {
            if (all_sessions.find(day_idx) == all_sessions.end()) {
                const uint64_t sample_ns = (trs.front().entry_time_ns > 0) ? trs.front().entry_time_ns : trs.front().exit_time_ns;
                all_sessions[day_idx] = format_ist_date(sample_ns);
            }
        }

        double running_capital = initial_capital_inr;
        size_t total_ks_triggers = 0;
        constexpr double kBreakevenBandInr = 0.05;

        for (const auto& [day_idx, date_str] : all_sessions) {
            DailySessionAudit day{};
            day.ist_day_index = day_idx;
            day.date_ist = date_str;
            day.starting_equity_inr = running_capital;

            const auto tr_it = session_trades.find(day_idx);
            const auto ks_it = ks_by_day.find(day_idx);

            if (tr_it != session_trades.end()) {
                const auto& trs = tr_it->second;
                day.total_trades = trs.size();

                double running_intra_pnl = 0.0;
                double peak_intra_pnl = 0.0;
                double max_intra_dd = 0.0;

                for (const auto& t : trs) {
                    day.gross_pnl_inr += t.gross_pnl_inr;
                    if (t.gross_pnl_inr > 0.0) {
                        day.gross_profit_inr += t.gross_pnl_inr;
                    } else if (t.gross_pnl_inr < 0.0) {
                        day.gross_loss_inr += std::abs(t.gross_pnl_inr);
                    }

                    day.stt_inr += t.stt_inr;
                    day.exchange_fee_inr += t.exchange_fee_inr;
                    day.gst_inr += t.gst_inr;
                    day.stamp_duty_inr += t.stamp_duty_inr;
                    day.sebi_turnover_inr += t.sebi_turnover_inr;
                    day.turnover_inr += t.turnover_inr();
                    day.net_pnl_inr += t.net_pnl_inr;

                    if (t.net_pnl_inr > kBreakevenBandInr) {
                        day.winning_trades++;
                    } else if (t.net_pnl_inr < -kBreakevenBandInr) {
                        day.losing_trades++;
                    } else {
                        day.breakeven_trades++;
                    }

                    // Intraday peak-to-trough drawdown across closed trades
                    running_intra_pnl += t.net_pnl_inr;
                    if (running_intra_pnl > peak_intra_pnl) {
                        peak_intra_pnl = running_intra_pnl;
                    }
                    const double intra_dd = peak_intra_pnl - running_intra_pnl;
                    if (intra_dd > max_intra_dd) {
                        max_intra_dd = intra_dd;
                    }
                }

                day.total_friction_inr = day.stt_inr + day.exchange_fee_inr + day.gst_inr +
                                        day.stamp_duty_inr + day.sebi_turnover_inr;
                day.win_rate_pct = (day.total_trades > 0)
                                       ? (static_cast<double>(day.winning_trades) / static_cast<double>(day.total_trades) * 100.0)
                                       : 0.0;

                if (ks_it != ks_by_day.end()) {
                    day.max_intraday_drawdown_inr = std::max(max_intra_dd, ks_it->second.max_drawdown_inr);
                    day.kill_switch_triggered =
                        (ks_it->second.status == risk::KillSwitchStatus::TRIPPED ||
                         day.max_intraday_drawdown_inr >= kill_switch_threshold_inr);
                } else {
                    day.max_intraday_drawdown_inr = max_intra_dd;
                    day.kill_switch_triggered = (max_intra_dd >= kill_switch_threshold_inr);
                }
            } else {
                // Zero-trade session
                day.total_trades = 0;
                if (ks_it != ks_by_day.end()) {
                    day.max_intraday_drawdown_inr = ks_it->second.max_drawdown_inr;
                    day.kill_switch_triggered = (ks_it->second.status == risk::KillSwitchStatus::TRIPPED);
                } else {
                    day.max_intraday_drawdown_inr = 0.0;
                    day.kill_switch_triggered = false;
                }
            }

            if (day.kill_switch_triggered) {
                total_ks_triggers++;
            }

            day.ending_equity_inr = day.starting_equity_inr + day.net_pnl_inr;
            running_capital = day.ending_equity_inr;

            report.daily_sessions.push_back(day);
        }

        // Construct equity curve points and high-water mark drawdown progression
        double hwm = initial_capital_inr;
        double max_cum_dd_inr = 0.0;
        double max_cum_dd_pct = 0.0;
        size_t profitable_days = 0;
        size_t losing_days = 0;
        size_t scratch_days = 0;

        for (size_t i = 0; i < report.daily_sessions.size(); ++i) {
            const auto& sa = report.daily_sessions[i];
            EquityCurvePoint pt{};
            pt.day_number = i + 1;
            pt.date_ist = sa.date_ist;
            pt.ist_day_index = sa.ist_day_index;
            pt.starting_equity_inr = sa.starting_equity_inr;
            pt.closing_equity_inr = sa.ending_equity_inr;
            pt.daily_net_pnl_inr = sa.net_pnl_inr;
            pt.trade_count = sa.total_trades;

            if (pt.starting_equity_inr > 0.0) {
                pt.daily_return_pct = (pt.daily_net_pnl_inr / pt.starting_equity_inr) * 100.0;
            }

            pt.cumulative_net_pnl_inr = pt.closing_equity_inr - initial_capital_inr;
            if (initial_capital_inr > 0.0) {
                pt.cumulative_return_pct = (pt.cumulative_net_pnl_inr / initial_capital_inr) * 100.0;
            }

            hwm = std::max(hwm, pt.closing_equity_inr);
            pt.high_water_mark_inr = hwm;

            pt.drawdown_from_hwm_inr = hwm - pt.closing_equity_inr;
            if (hwm > 0.0) {
                pt.drawdown_from_hwm_pct = (pt.drawdown_from_hwm_inr / hwm) * 100.0;
            }

            max_cum_dd_inr = std::max(max_cum_dd_inr, pt.drawdown_from_hwm_inr);
            max_cum_dd_pct = std::max(max_cum_dd_pct, pt.drawdown_from_hwm_pct);

            if (pt.daily_net_pnl_inr > kBreakevenBandInr) {
                profitable_days++;
            } else if (pt.daily_net_pnl_inr < -kBreakevenBandInr) {
                losing_days++;
            } else {
                scratch_days++;
            }

            report.equity_curve.push_back(pt);
        }

        // Populate kill_switch_history if provided, else synthesize from daily sessions
        if (!kill_switch_history.empty()) {
            report.kill_switch_history = kill_switch_history;
        } else {
            for (const auto& sa : report.daily_sessions) {
                risk::DailyKillSwitchAudit ks{};
                ks.date_ist = sa.date_ist;
                ks.ist_day_index = sa.ist_day_index;
                ks.starting_capital_inr = sa.starting_equity_inr;
                ks.peak_capital_inr = std::max(sa.starting_equity_inr, sa.ending_equity_inr);
                ks.trough_capital_inr = std::min(sa.starting_equity_inr, sa.ending_equity_inr);
                ks.max_drawdown_inr = sa.max_intraday_drawdown_inr;
                ks.max_drawdown_pct = (ks.peak_capital_inr > 0.0) ? (ks.max_drawdown_inr / ks.peak_capital_inr * 100.0) : 0.0;
                ks.status = sa.kill_switch_triggered ? risk::KillSwitchStatus::TRIPPED : risk::KillSwitchStatus::CLEAR;
                ks.trade_count = sa.total_trades;
                report.kill_switch_history.push_back(std::move(ks));
            }
        }

        // Fill aggregate metrics
        report.aggregate.ending_capital_inr = running_capital;
        report.aggregate.net_return_pct =
            (initial_capital_inr > 0.0) ? ((running_capital - initial_capital_inr) / initial_capital_inr * 100.0) : 0.0;
        report.aggregate.peak_capital_inr = hwm;
        report.aggregate.max_portfolio_drawdown_inr = max_cum_dd_inr;
        report.aggregate.max_portfolio_drawdown_pct = max_cum_dd_pct;
        report.aggregate.total_sessions = report.daily_sessions.size();
        report.aggregate.total_kill_switch_triggers = total_ks_triggers;
        report.aggregate.profitable_days = profitable_days;
        report.aggregate.losing_days = losing_days;
        report.aggregate.scratch_days = scratch_days;
        report.aggregate.daily_win_rate_pct =
            (report.aggregate.total_sessions > 0)
                ? (static_cast<double>(profitable_days) / static_cast<double>(report.aggregate.total_sessions) * 100.0)
                : 0.0;

        return report;
    }

    [[nodiscard]] static MultiDayAuditReport evaluate_multi_day(
        const std::vector<core::TradeAuditEntry>& trades,
        const std::vector<risk::DailyKillSwitchAudit>& kill_switch_history,
        double initial_capital_inr = 10000.0) {
        return evaluate_multi_day(trades, initial_capital_inr, {}, kill_switch_history, 500.0);
    }

    [[nodiscard]] MultiDayAuditReport generate_report(
        const std::vector<core::TradeAuditEntry>& trades,
        const std::vector<risk::DailyKillSwitchAudit>& kill_switch_history = {},
        double kill_switch_threshold_inr = 500.0) const {
        return evaluate_multi_day(trades, initial_capital_inr_, registered_sessions_, kill_switch_history, kill_switch_threshold_inr);
    }

    static void print_report(const AuditSummary& s, std::ostream& out = std::cout) {
        out << "\n============================================================\n";
        out << "              DELTAUNO PERFORMANCE AUDIT REPORT             \n";
        out << "============================================================\n";
        out << std::fixed << std::setprecision(2);
        out << "  Total Completed Trades    : " << s.total_trades << "\n";
        out << "  Winning Trades            : " << s.winning_trades << " (" << s.win_rate_pct << "%)\n";
        out << "  Losing Trades             : " << s.losing_trades << "\n";
        out << "  Break-Even Trades         : " << s.breakeven_trades << "\n";
        out << "------------------------------------------------------------\n";
        out << "  Gross Profit              : +" << s.gross_profit_inr << " INR\n";
        out << "  Gross Loss                : -" << s.gross_loss_inr << " INR\n";
        out << "  Total Statutory Friction  : -" << s.total_taxes_and_friction_inr << " INR (STT/GST/Turnover/SEBI)\n";
        out << "  Net PnL                   : " << (s.net_pnl_inr >= 0.0 ? "+" : "") << s.net_pnl_inr << " INR\n";
        out << "  Profit Factor             : " << s.profit_factor << "\n";
        out << "  Expectancy / Trade        : " << (s.expectancy_per_trade_inr >= 0.0 ? "+" : "")
            << s.expectancy_per_trade_inr << " INR\n";
        out << "  Friction Drag Ratio       : " << s.friction_drag_ratio_pct << "% of gross profit\n";
        out << "------------------------------------------------------------\n";
        out << "  Avg MAE (Adverse Move)    : " << (s.avg_mae_paise / 100.0) << " INR\n";
        out << "  Avg MFE (Favorable Move)  : " << (s.avg_mfe_paise / 100.0) << " INR\n";
        out << "============================================================\n\n";
    }

    static void print_statutory_friction_table(const AuditSummary& s, std::ostream& out = std::cout) {
        out << "\n----------------------------------------------------------------------------------------------------------------------------\n";
        out << "                                         ITEMIZED STATUTORY CHARGES AUDIT (INDIAN TAXES)                                    \n";
        out << "----------------------------------------------------------------------------------------------------------------------------\n";
        out << "  Statutory Regulatory Charge         Official Basis / Statutory Rate                           Deduction (INR)   Share %   \n";
        out << "----------------------------------------------------------------------------------------------------------------------------\n";
        out << std::fixed << std::setprecision(2);

        const double total_fric = s.total_taxes_and_friction_inr > 0.0 ? s.total_taxes_and_friction_inr : 1.0;
        const double stt_pct = (s.total_stt_inr / total_fric) * 100.0;
        const double exch_pct = (s.total_exchange_fee_inr / total_fric) * 100.0;
        const double gst_pct = (s.total_gst_inr / total_fric) * 100.0;
        const double stamp_pct = (s.total_stamp_duty_inr / total_fric) * 100.0;
        const double sebi_pct = (s.total_sebi_turnover_inr / total_fric) * 100.0;

        out << "  Securities Transaction Tax (STT)    0.025% on Intraday Sell Turnover (Cash MIS)                 -INR "
            << std::setw(8) << s.total_stt_inr << "    " << std::setw(5) << std::setprecision(1) << stt_pct << "%\n";
        out << "  NSE Transaction Turnover Charges    0.00297% on Total Traded Turnover (Buy + Sell)               -INR "
            << std::setw(8) << std::setprecision(2) << s.total_exchange_fee_inr << "    " << std::setw(5) << std::setprecision(1) << exch_pct << "%\n";
        out << "  Goods & Services Tax (GST)          18.0% on Exchange + SEBI Charges                             -INR "
            << std::setw(8) << std::setprecision(2) << s.total_gst_inr << "    " << std::setw(5) << std::setprecision(1) << gst_pct << "%\n";
        out << "  State Stamp Duty                    0.003% on Buy Leg Turnover Notional                          -INR "
            << std::setw(8) << std::setprecision(2) << s.total_stamp_duty_inr << "    " << std::setw(5) << std::setprecision(1) << stamp_pct << "%\n";
        out << "  SEBI Regulatory Turnover Fee        INR 10 per Crore of Total Traded Turnover (0.0001%)          -INR "
            << std::setw(8) << std::setprecision(2) << s.total_sebi_turnover_inr << "    " << std::setw(5) << std::setprecision(1) << sebi_pct << "%\n";
        out << "----------------------------------------------------------------------------------------------------------------------------\n";
        out << "  TOTAL STATUTORY FRICTION DRAG       Strict Indian Regulatory Clearing Parity                     -INR "
            << std::setw(8) << std::setprecision(2) << s.total_taxes_and_friction_inr << "   100.0%\n";
        out << "  TOTAL TRADED TURNOVER (NOTIONAL)    Cumulative Buy Notional + Sell Notional                       INR "
            << std::setw(8) << std::setprecision(2) << s.total_turnover_inr << "\n";
        out << "  FRICTION DRAG RATIO ON PROFIT       "
            << std::setw(5) << std::setprecision(2) << s.friction_drag_ratio_pct << "% of Gross Trading Profit\n";
        out << "----------------------------------------------------------------------------------------------------------------------------\n";
    }

    static void print_multi_day_report(const MultiDayAuditReport& report, std::ostream& out = std::cout) {
        const auto& s = report.aggregate;
        out << "\n============================================================================================================================\n";
        out << "                               DELTAUNO INSTITUTIONAL PERFORMANCE AUDITOR (30-DAY REPLAY)                                   \n";
        out << "============================================================================================================================\n";
        out << std::fixed << std::setprecision(2);
        out << "  Initial Capital     : INR " << std::setw(10) << s.initial_capital_inr
            << "          Replay Horizon      : " << s.total_sessions << " Sessions ("
            << (report.daily_sessions.empty() ? "" : report.daily_sessions.front().date_ist + " to " + report.daily_sessions.back().date_ist) << ")\n";
        out << "  Ending Capital      : INR " << std::setw(10) << s.ending_capital_inr
            << "          Execution Mandate   : NSE Intraday MIS Equities (Zero-Loss)\n";
        out << "  Net Portfolio PnL   : " << (s.net_pnl_inr >= 0 ? "+" : "") << s.net_pnl_inr << " INR ("
            << (s.net_return_pct >= 0 ? "+" : "") << std::setprecision(2) << s.net_return_pct << "%)"
            << "   Risk Shields Model  : 13 Shields (INR 120 SL, INR 500 KS, 15:05 Auto-Squareoff)\n";
        out << "============================================================================================================================\n";

        // 1. Session Breakdown Table
        out << "\n----------------------------------------------------------------------------------------------------------------------------\n";
        out << "                                      30-DAY SESSION-BY-SESSION PERFORMANCE BREAKDOWN                                       \n";
        out << "----------------------------------------------------------------------------------------------------------------------------\n";
        out << " Day  Date (IST)   Trades  W / L / B  Win Rate     Gross PnL      Friction       Net PnL   Intra DD     Ending Equity  Kill-Switch\n";
        out << "----------------------------------------------------------------------------------------------------------------------------\n";

        double sum_gross = 0.0;
        double sum_friction = 0.0;
        double sum_net = 0.0;
        size_t total_trades = 0;
        size_t total_wins = 0;
        size_t total_losses = 0;
        size_t total_scratches = 0;
        double max_seen_intra_dd = 0.0;

        for (size_t i = 0; i < report.daily_sessions.size(); ++i) {
            const auto& d = report.daily_sessions[i];
            sum_gross += d.gross_pnl_inr;
            sum_friction += d.total_friction_inr;
            sum_net += d.net_pnl_inr;
            total_trades += d.total_trades;
            total_wins += d.winning_trades;
            total_losses += d.losing_trades;
            total_scratches += d.breakeven_trades;
            if (d.max_intraday_drawdown_inr > max_seen_intra_dd) {
                max_seen_intra_dd = d.max_intraday_drawdown_inr;
            }

            const std::string wlb = std::to_string(d.winning_trades) + "/" +
                                    std::to_string(d.losing_trades) + "/" +
                                    std::to_string(d.breakeven_trades);

            std::ostringstream ss_gross, ss_fric, ss_net, ss_dd, ss_eq;
            ss_gross << std::fixed << std::setprecision(2) << (d.gross_pnl_inr >= 0.0 ? "+" : "") << d.gross_pnl_inr << " INR";
            ss_fric << std::fixed << std::setprecision(2) << "-" << d.total_friction_inr << " INR";
            ss_net << std::fixed << std::setprecision(2) << (d.net_pnl_inr >= 0.0 ? "+" : "") << d.net_pnl_inr << " INR";
            ss_dd << std::fixed << std::setprecision(2) << d.max_intraday_drawdown_inr << " INR";
            ss_eq << std::fixed << std::setprecision(2) << d.ending_equity_inr << " INR";

            out << "  " << std::setw(2) << (i + 1) << "  "
                << d.date_ist << "    "
                << std::setw(4) << d.total_trades << "   "
                << std::setw(7) << wlb << "   "
                << std::setw(6) << std::setprecision(1) << d.win_rate_pct << "%   "
                << std::setw(13) << ss_gross.str() << "  "
                << std::setw(12) << ss_fric.str() << "  "
                << std::setw(13) << ss_net.str() << " "
                << std::setw(10) << ss_dd.str() << "  "
                << std::setw(14) << ss_eq.str() << "    "
                << (d.kill_switch_triggered ? "TRIGGERED" : "  SAFE  ") << "\n";
        }

        out << "----------------------------------------------------------------------------------------------------------------------------\n";
        const std::string ttl_wlb = std::to_string(total_wins) + "/" + std::to_string(total_losses) + "/" + std::to_string(total_scratches);
        const double ttl_win_rate = total_trades > 0 ? (static_cast<double>(total_wins) / static_cast<double>(total_trades) * 100.0) : 0.0;

        std::ostringstream ss_tot_gross, ss_tot_fric, ss_tot_net, ss_tot_dd, ss_tot_eq;
        ss_tot_gross << std::fixed << std::setprecision(2) << (sum_gross >= 0.0 ? "+" : "") << sum_gross << " INR";
        ss_tot_fric << std::fixed << std::setprecision(2) << "-" << sum_friction << " INR";
        ss_tot_net << std::fixed << std::setprecision(2) << (sum_net >= 0.0 ? "+" : "") << sum_net << " INR";
        ss_tot_dd << std::fixed << std::setprecision(2) << max_seen_intra_dd << " Max";
        ss_tot_eq << std::fixed << std::setprecision(2) << s.ending_capital_inr << " INR";

        out << " TTL                 "
            << std::setw(4) << total_trades << "   "
            << std::setw(7) << ttl_wlb << "   "
            << std::setw(6) << std::setprecision(1) << ttl_win_rate << "%   "
            << std::setw(13) << ss_tot_gross.str() << "  "
            << std::setw(12) << ss_tot_fric.str() << "  "
            << std::setw(13) << ss_tot_net.str() << " "
            << std::setw(10) << ss_tot_dd.str() << "  "
            << std::setw(14) << ss_tot_eq.str() << "    "
            << (s.total_kill_switch_triggers > 0 ? std::to_string(s.total_kill_switch_triggers) + " TRIG" : " 0 TRIG ") << "\n";
        out << "----------------------------------------------------------------------------------------------------------------------------\n";

        // 2. Itemized Statutory Friction Table
        print_statutory_friction_table(s, out);

        // 3. Daily Kill-Switch Audit History Table
        out << "\n============================================================================================================================\n";
        out << "                                        DAILY KILL-SWITCH & DRAWDOWN AUDIT TRAIL                                            \n";
        out << "============================================================================================================================\n";
        out << " Day  Date (IST)   Trades   Starting (INR)    Peak Equity (INR)  Trough Equity (INR)  Max Intraday DD (INR & %)      Status \n";
        out << "----------------------------------------------------------------------------------------------------------------------------\n";
        size_t tripped_count = 0;
        double worst_intraday_dd = 0.0;
        double worst_intraday_dd_pct = 0.0;

        for (size_t i = 0; i < report.kill_switch_history.size(); ++i) {
            const auto& ks = report.kill_switch_history[i];
            if (ks.status == risk::KillSwitchStatus::TRIPPED) tripped_count++;
            if (ks.max_drawdown_inr > worst_intraday_dd) {
                worst_intraday_dd = ks.max_drawdown_inr;
                worst_intraday_dd_pct = ks.max_drawdown_pct;
            }

            std::ostringstream ss_dd_pct;
            ss_dd_pct << std::fixed << std::setprecision(2) << ks.max_drawdown_inr << " (" << std::setprecision(2) << ks.max_drawdown_pct << "%)";

            out << "  " << std::setw(2) << (i + 1) << "  "
                << ks.date_ist << "    "
                << std::setw(4) << ks.trade_count << "     "
                << std::setw(14) << std::fixed << std::setprecision(2) << ks.starting_capital_inr << "     "
                << std::setw(14) << ks.peak_capital_inr << "      "
                << std::setw(14) << ks.trough_capital_inr << "   "
                << std::setw(22) << ss_dd_pct.str() << "      "
                << std::setw(7) << risk::kill_switch_status_to_string(ks.status) << "\n";
        }
        out << "----------------------------------------------------------------------------------------------------------------------------\n";
        out << " Summary: " << report.kill_switch_history.size() << " Sessions Audited | "
            << tripped_count << " Kill-Switch Tripped | Worst Intraday Drawdown: "
            << worst_intraday_dd << " INR (" << std::setprecision(2) << worst_intraday_dd_pct << "%)\n";
        out << "============================================================================================================================\n";

        // 4. 30-Day Portfolio Equity Curve Progression Table
        out << "\n============================================================================================================================\n";
        out << "                                            30-DAY PORTFOLIO EQUITY CURVE PROGRESSION                                       \n";
        out << "============================================================================================================================\n";
        out << " Day  Date (IST)   Trades  Closing Equity (INR)   Daily Return %   Cum. Return %   High-Water Mark (INR)   Drawdown from HWM\n";
        out << "----------------------------------------------------------------------------------------------------------------------------\n";
        for (const auto& pt : report.equity_curve) {
            std::ostringstream ss_d_ret, ss_c_ret, ss_dd_hwm;
            ss_d_ret << std::fixed << std::setprecision(2) << (pt.daily_return_pct >= 0.0 ? "+" : "") << pt.daily_return_pct << "%";
            ss_c_ret << std::fixed << std::setprecision(2) << (pt.cumulative_return_pct >= 0.0 ? "+" : "") << pt.cumulative_return_pct << "%";
            ss_dd_hwm << std::fixed << std::setprecision(2) << pt.drawdown_from_hwm_inr << " (" << pt.drawdown_from_hwm_pct << "%)";

            out << "  " << std::setw(2) << pt.day_number << "  "
                << pt.date_ist << "    "
                << std::setw(4) << pt.trade_count << "    "
                << std::setw(16) << std::fixed << std::setprecision(2) << pt.closing_equity_inr << "     "
                << std::setw(12) << ss_d_ret.str() << "     "
                << std::setw(12) << ss_c_ret.str() << "     "
                << std::setw(16) << pt.high_water_mark_inr << "      "
                << std::setw(18) << ss_dd_hwm.str() << "\n";
        }
        out << "============================================================================================================================\n";

        // 5. Executive Summary Statistics
        out << "\n----------------------------------------------------------------------------------------------------------------------------\n";
        out << "                                        COMPREHENSIVE 30-DAY STATISTICAL METRICS                                            \n";
        out << "----------------------------------------------------------------------------------------------------------------------------\n";
        out << "  Total Completed Trades    : " << std::setw(6) << s.total_trades
            << "                       Win Rate                   : " << std::setw(6) << std::setprecision(2) << s.win_rate_pct << "%\n";
        out << "  Winning Trades            : " << std::setw(6) << s.winning_trades
            << "                       Average MAE (Adverse Move) : " << std::setw(6) << (s.avg_mae_paise / 100.0) << " INR (" << s.avg_mae_paise << " paise)\n";
        out << "  Losing Trades             : " << std::setw(6) << s.losing_trades
            << "                       Average MFE (Favorable Move: " << std::setw(6) << (s.avg_mfe_paise / 100.0) << " INR (" << s.avg_mfe_paise << " paise)\n";
        out << "  Break-Even Scratch Trades : " << std::setw(6) << s.breakeven_trades
            << "                       Trade Expectancy           : " << (s.expectancy_per_trade_inr >= 0 ? "+" : "") << s.expectancy_per_trade_inr << " INR / trade\n";
        out << "  Gross Trading Profit      : +" << std::setw(8) << s.gross_profit_inr << " INR"
            << "                Gross Trading Loss         : -" << std::setw(8) << s.gross_loss_inr << " INR\n";
        out << "  Profit Factor             : " << std::setw(6) << s.profit_factor
            << "                       Max Intraday Drawdown      : " << std::setw(6) << max_seen_intra_dd << " INR (Limit: INR 500.00)\n";
        out << "  Max Portfolio Drawdown    : " << std::setw(6) << s.max_portfolio_drawdown_inr << " INR (" << std::setprecision(2) << s.max_portfolio_drawdown_pct << "%)"
            << "       Daily Kill-Switch Activations: " << s.total_kill_switch_triggers << " / " << s.total_sessions << " Sessions (100% SAFE)\n";
        out << "  Active Trading Sessions   : " << std::setw(6) << s.total_sessions
            << "                       Profitable / Loss / Flat   : " << s.profitable_days << " / " << s.losing_days << " / " << s.scratch_days << "\n";
        out << "============================================================================================================================\n\n";
    }

    static void print_report(const MultiDayAuditReport& report, std::ostream& out = std::cout) {
        print_multi_day_report(report, out);
    }

private:
    double initial_capital_inr_{10000.0};
    std::vector<std::pair<int64_t, std::string>> registered_sessions_;
};

} // namespace deltauno::execution
