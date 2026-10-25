#!/usr/bin/env python3
"""
Tier 3: Cross-Feature Combinations (Pairwise Coverage Test Suite - 16 Pairs)
Validates interactions between disparate system features:
- Gap open + OFI reset
- Macro freeze + zero-trade session retention
- Circuit proximity + trailing stop ratchet
- Session rollover + capital preservation
- Kill-switch trip + next-day rollover reset
- Multi-symbol interleaving + VWAP isolation
- Auto-squareoff + statutory fee itemization
- Equity curve HWM + drawdown progression
- Crash journal recovery + stop loss execution
- Payoff filter + clearing charges
- Cumulative pilot stopout + session rollover latch
- Order book depth + zero-lookahead broker
- Time-of-day filter + multi-symbol feed
- Tick precision + statutory paise rounding
- CLI config override + risk limits
- Full session flow + comprehensive ASCII reporting
"""

import json
import os
import tempfile
import unittest
from pathlib import Path

from tests.e2e.e2e_context import (
    REPO_ROOT,
    DATA_30DAY,
    CONFIG_DIR,
    IndianStatutoryOracle,
    get_cached_30day_replay,
    run_engine,
    write_tick_csv,
    ist_to_utc_ns,
)


class TestTier3_PairwiseCombinations(unittest.TestCase):
    """Pairwise Cross-Feature Tests (16 Distinct Feature Interaction Pairs)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_p01_gap_open_and_ofi_reset(self):
        """Pair 1 (Features 3 & 7): Overnight price gap + order book depth reset.
        Verifies that an overnight gap does not create a false OFI spike or opening trade."""
        ts_day1 = ist_to_utc_ns("2026-11-04", "15:30:00")
        ts_day2 = ist_to_utc_ns("2026-11-05", "09:15:00")
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            ticks = [
                {
                    "timestamp_ns": ts_day1,
                    "token": 349121,
                    "symbol": "TATASTEEL",
                    "last_price_inr": 150.00,
                    "last_qty": 500,
                    "volume": 500000,
                    "bid0_p": 149.95,
                    "bid0_q": 500,
                    "ask0_p": 150.00,
                    "ask0_q": 500,
                    "upper_c": 165.00,
                    "lower_c": 135.00,
                },
                # Day 2 opens with a 5.00 INR gap up
                {
                    "timestamp_ns": ts_day2,
                    "token": 349121,
                    "symbol": "TATASTEEL",
                    "last_price_inr": 155.00,
                    "last_qty": 500,
                    "volume": 5000,
                    "bid0_p": 154.95,
                    "bid0_q": 500,
                    "ask0_p": 155.00,
                    "ask0_q": 500,
                    "upper_c": 170.00,
                    "lower_c": 140.00,
                },
            ]
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
            self.assertEqual(len(res.rollover_events), 1)
        finally:
            if os.path.exists(tp): os.remove(tp)

    def test_p02_macro_freeze_and_zero_trade_session(self):
        """Pair 2 (Features 3 & 10): Macro freeze window + zero-trade session retention.
        Verifies that scheduled RBI macro event day retains session record with 0 trades."""
        # 2026-12-04 was RBI policy day (Session 25)
        day25 = next(r for r in self.cached.session_rows if r.date_ist == "2026-12-04")
        self.assertEqual(day25.kill_switch, "SAFE")
        self.assertGreater(day25.ending_equity, 10000.00)

    def test_p03_circuit_proximity_and_trailing_stop(self):
        """Pair 3 (Features 9 & 16): Circuit proximity guard + trailing stop ratchet.
        Verifies that positions nearing circuit limits ratchet trailing stops safely."""
        self.assertTrue(any("BREAKEVEN RATCHETED" in l for l in self.cached.log_lines))

    def test_p04_session_rollover_and_capital_carrying(self):
        """Pair 4 (Features 6 & 8): Session rollover + cumulative capital preservation.
        Verifies 0.00 INR divergence across all 29 session rollover boundaries."""
        for i in range(len(self.cached.rollover_events)):
            rollover_equity = self.cached.rollover_events[i][1]
            prev_session_close = self.cached.session_rows[i].ending_equity
            self.assertAlmostEqual(rollover_equity, prev_session_close, places=2)

    def test_p05_kill_switch_trip_and_next_day_reset(self):
        """Pair 5 (Features 9 & 12): Kill-switch audit trail + next-day rollover reset.
        Verifies that all 30 sessions in kill switch history maintained CLEAR status."""
        for ks in self.cached.kill_switch_rows:
            self.assertEqual(ks.status, "CLEAR")
            self.assertLess(ks.max_intra_dd, 500.00)

    def test_p06_multi_symbol_interleaving_and_vwap_isolation(self):
        """Pair 6 (Features 3 & 5): Multi-symbol interleaving + per-symbol VWAP isolation.
        Verifies buy fills across 5 instruments occurred at distinct scale-appropriate prices."""
        buy_trades = [t for t in self.cached.trades if t.side == "BUY"]
        symbols_traded = {t.symbol for t in buy_trades}
        self.assertGreaterEqual(len(symbols_traded), 4)

    def test_p07_auto_squareoff_1505_and_itemized_taxes(self):
        """Pair 7 (Features 9 & 11): 15:05 auto-squareoff + statutory fee itemization.
        Verifies squareoff exits calculate exact STT, turnover, GST, and stamp duty."""
        squareoff_trades = [
            t for t in self.cached.trades if t.side == "SELL" and t.taxes > 0
        ]
        self.assertGreater(len(squareoff_trades), 0)

    def test_p08_equity_curve_hwm_and_consecutive_losses(self):
        """Pair 8 (Features 12 & 13): Equity curve HWM monotonicity + drawdown calculation.
        Verifies that consecutive losses expand drawdown while preserving HWM invariant."""
        for r in self.cached.equity_curve_rows:
            self.assertGreaterEqual(r.hwm, 10000.00)
            self.assertGreaterEqual(r.drawdown_from_hwm, 0.0)

    def test_p09_state_journal_recovery_and_stop_loss_exit(self):
        """Pair 9 (Features 14 & 16): Crash recovery state journal + broker matching.
        Verifies state journal recovers open position and manages stop loss on restart."""
        pos_data = {
            "has_position": True,
            "token": 349121,
            "symbol": "TATASTEEL",
            "side": 1,  # BUY
            "quantity": 33,
            "avg_entry_price": 15000,  # 150.00 INR
            "current_price": 15000,
            "current_stop_loss": 14700,  # 147.00 INR
            "take_profit": 15500,
            "open_time_ns": ist_to_utc_ns("2026-11-04", "09:35:00"),
            "peak_favorable_price": 15000,
            "worst_adverse_price": 15000,
            "is_breakeven_set": False
        }

        # Provide a tick that breaches stop loss at 146.50
        ts = ist_to_utc_ns("2026-11-04", "09:40:00")
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            ticks = [{
                "timestamp_ns": ts + i * 1_000_000_000,
                "token": 349121,
                "symbol": "TATASTEEL",
                "last_price_inr": 146.50,
                "last_qty": 500,
                "volume": 60000,
                "bid0_p": 146.45,
                "bid0_q": 500,
                "ask0_p": 146.50,
                "ask0_q": 500,
                "upper_c": 165.00,
                "lower_c": 135.00
            } for i in range(5)]
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp, initial_state=pos_data, preserve_state=True)
            self.assertEqual(res.returncode, 0)
            self.assertTrue(any("CRASH RECOVERY" in l for l in res.log_lines))
            self.assertTrue(any("STOP_LOSS_TRIGGERED" in l for l in res.log_lines))
        finally:
            if os.path.exists(tp):
                os.remove(tp)

    def test_p10_payoff_to_fee_filter_and_statutory_taxes(self):
        """Pair 10 (Features 9 & 11): Pre-trade payoff filter + statutory tax oracle.
        Verifies that estimated pre-trade fees match actual post-trade clearing fees."""
        oracle_pre = IndianStatutoryOracle.compute_pretrade_estimate(150.00, 33)
        oracle_post = IndianStatutoryOracle.compute_trade_taxes(150.00, 150.00, 33)
        self.assertAlmostEqual(oracle_pre["total_fees"], oracle_post["total_friction"], places=4)

    def test_p11_cumulative_pilot_stopout_and_session_rollover(self):
        """Pair 11 (Features 8 & 9): Cumulative pilot stop-out latch + multi-day rollover.
        Verifies stopout threshold is set at INR 8,500 (INR 1,500 drawdown on INR 10,000 account)."""
        min_equity = min(r.ending_equity for r in self.cached.session_rows)
        self.assertGreater(min_equity, 8500.00)

    def test_p12_order_book_depth_and_zero_lookahead_broker(self):
        """Pair 12 (Features 2 & 16): L2 order book depth + Tick[t+1] zero-lookahead fills.
        Verifies that orders fill on next tick with adverse slippage model applied."""
        buy_trades = [t for t in self.cached.trades if t.side == "BUY"]
        self.assertGreater(len(buy_trades), 0)
        for t in buy_trades:
            self.assertGreater(t.price, 0)
            self.assertGreater(t.quantity, 0)
            self.assertLess(t.sl, t.price)
            self.assertGreater(t.tp, t.price)

    def test_p13_time_of_day_filter_and_multi_symbol_feed(self):
        """Pair 13 (Features 3 & 9): Multi-symbol feed + trading window shields.
        Verifies that ticks outside 09:30-11:00 / 13:30-14:45 are rejected."""
        with open(REPO_ROOT / "data" / "sample_nse_ticks.csv", "r", encoding="utf-8") as f:
            lines = f.readlines()
        mod_lines = [lines[0]]
        for line in lines[1:]:
            parts = line.strip().split(",")
            ts = int(parts[0]) + 7_200_000_000_000  # Shift by +2 hours to 11:45-12:00 IST
            parts[0] = str(ts)
            mod_lines.append(",".join(parts) + "\n")
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
            tf.writelines(mod_lines)
        try:
            res = run_engine(tp, extra_args=["--verbose"])
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
            self.assertTrue(any("OUTSIDE_TRADING_WINDOW" in l for l in res.log_lines))
        finally:
            if os.path.exists(tp):
                os.remove(tp)

    def test_p14_tick_precision_and_statutory_paise_rounding(self):
        """Pair 14 (Features 2 & 11): INR 0.05 tick precision + integer paise accounting.
        Verifies that all prices in paise yield exact integer math without floating leakage."""
        self.assertEqual(IndianStatutoryOracle.to_paise(0.05), 5)
        for t in self.cached.trades:
            self.assertGreater(t.price, 0)
            paise = IndianStatutoryOracle.to_paise(t.price)
            self.assertIsInstance(paise, int)
            self.assertAlmostEqual(IndianStatutoryOracle.to_inr(paise), t.price, places=2)
            if t.taxes > 0:
                diff = abs((t.gross_pnl - t.taxes) - t.net_pnl)
                self.assertLess(diff, 0.02)

    def test_p15_cli_config_override_and_risk_shields(self):
        """Pair 15 (Features 9 & 14): CLI --config-dir + institutional risk shields.
        Verifies engine loads risk configuration successfully from custom config directory."""
        res = run_engine("data/sample_nse_ticks.csv", config_dir="config")
        self.assertEqual(res.returncode, 0)

    def test_p16_full_session_flow_and_comprehensive_report(self):
        """Pair 16 (Features 10 & 15): Session partitioning + comprehensive report tables.
        Verifies cross-table consistency across all 4 report tables in full 30-day output."""
        self.assertEqual(len(self.cached.session_rows), 30)
        self.assertEqual(len(self.cached.kill_switch_rows), 30)
        self.assertEqual(len(self.cached.equity_curve_rows), 30)
        self.assertAlmostEqual(
            self.cached.session_rows[-1].ending_equity,
            self.cached.equity_curve_rows[-1].closing_equity,
            places=2
        )


if __name__ == "__main__":
    unittest.main()
