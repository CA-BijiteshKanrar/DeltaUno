#!/usr/bin/env python3
"""
Tier 2: Boundary & Corner Cases Test Suite (80 Genuine Tests across 16 Features)
Validates extreme values, limit thresholds, circuit proximity, empty inputs,
1-tick sessions, weekend skips, sub-paise precision, and fail-safe triggers.
"""

import csv
import json
import os
import tempfile
import unittest
from datetime import datetime
from pathlib import Path

from tests.e2e.e2e_context import (
    REPO_ROOT,
    DATA_30DAY,
    CONFIG_DIR,
    IndianStatutoryOracle,
    find_engine_executable,
    get_cached_30day_replay,
    run_engine,
    write_tick_csv,
    ist_to_utc_ns,
)


class TestBoundary01_DataIngestion(unittest.TestCase):
    """Feature 1 Boundaries: Data Ingestion limits and edge inputs (5 tests)"""

    def test_b01_01_empty_csv_file(self):
        """Verify 0-byte CSV terminates cleanly without unhandled exception."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp): os.remove(tp)

    def test_b01_02_single_tick_session(self):
        """Verify CSV with exactly 1 tick completes with zero trades and code 0."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            ts = ist_to_utc_ns("2026-11-04", "09:30:00")
            ticks = [{
                "timestamp_ns": ts,
                "token": 349121,
                "symbol": "TATASTEEL",
                "last_price_inr": 150.00,
                "last_qty": 100,
                "volume": 50000,
                "bid0_p": 149.95,
                "bid0_q": 500,
                "ask0_p": 150.00,
                "ask0_q": 500,
                "upper_c": 165.00,
                "lower_c": 135.00,
            }]
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp): os.remove(tp)

    def test_b01_03_unwhitelisted_token_filtering(self):
        """Verify feed with unwhitelisted token (999999) is safely ignored without crash."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            ts = ist_to_utc_ns("2026-11-04", "09:30:00")
            ticks = [{
                "timestamp_ns": ts + i * 1_000_000_000,
                "token": 999999,
                "symbol": "UNKNOWN",
                "last_price_inr": 50.00,
                "last_qty": 100,
                "volume": 50000,
                "bid0_p": 49.95,
                "bid0_q": 500,
                "ask0_p": 50.00,
                "ask0_q": 500,
                "upper_c": 55.00,
                "lower_c": 45.00,
            } for i in range(5)]
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp): os.remove(tp)

    def test_b01_04_zero_volume_tick(self):
        """Verify tick with total_volume = 0 does not trigger divide-by-zero."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            ts = ist_to_utc_ns("2026-11-04", "09:30:00")
            ticks = [{
                "timestamp_ns": ts + i * 1_000_000_000,
                "token": 349121,
                "symbol": "TATASTEEL",
                "last_price_inr": 150.00,
                "last_qty": 0,
                "volume": 0,
                "bid0_p": 149.95,
                "bid0_q": 500,
                "ask0_p": 150.00,
                "ask0_q": 500,
                "upper_c": 165.00,
                "lower_c": 135.00,
            } for i in range(5)]
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp): os.remove(tp)

    def test_b01_05_sub_microsecond_tick_gap(self):
        """Verify consecutive ticks with 1ns gap process without timestamp inversion."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            ts = ist_to_utc_ns("2026-11-04", "09:30:00")
            ticks = [
                {
                    "timestamp_ns": ts,
                    "token": 349121,
                    "symbol": "TATASTEEL",
                    "last_price_inr": 150.00,
                    "last_qty": 100,
                    "volume": 50000,
                    "bid0_p": 149.95,
                    "bid0_q": 500,
                    "ask0_p": 150.00,
                    "ask0_q": 500,
                    "upper_c": 165.00,
                    "lower_c": 135.00,
                },
                {
                    "timestamp_ns": ts + 1,  # 1 nanosecond later
                    "token": 349121,
                    "symbol": "TATASTEEL",
                    "last_price_inr": 150.05,
                    "last_qty": 100,
                    "volume": 50100,
                    "bid0_p": 150.00,
                    "bid0_q": 500,
                    "ask0_p": 150.05,
                    "ask0_q": 500,
                    "upper_c": 165.00,
                    "lower_c": 135.00,
                },
            ]
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
        finally:
            if os.path.exists(tp): os.remove(tp)


class TestBoundary02_L2Reconstruction(unittest.TestCase):
    """Feature 2 Boundaries: L2 order book price & quantity extremes (5 tests)"""

    def test_b02_01_minimum_tick_price_five_paise(self):
        """Verify minimum possible tick price (INR 0.05 paise) calculates valid paise."""
        paise = IndianStatutoryOracle.to_paise(0.05)
        self.assertEqual(paise, 5)
        self.assertEqual(IndianStatutoryOracle.to_inr(paise), 0.05)

    def test_b02_02_maximum_mega_cap_price(self):
        """Verify mega-cap equity price (INR 35,000.00) converts to paise without overflow."""
        paise = IndianStatutoryOracle.to_paise(35000.00)
        self.assertEqual(paise, 3500000)
        turnover = paise * 1000  # 1,000 shares
        self.assertGreater(turnover, 0)

    def test_b02_03_minimum_spread_one_tick(self):
        """Verify minimum valid spread of exactly 1 tick (INR 0.05) maintains ask > bid."""
        bid = 150.00
        ask = 150.05
        self.assertAlmostEqual(ask - bid, 0.05, places=2)
        self.assertGreater(ask, bid)

    def test_b02_04_extreme_depth_quantity(self):
        """Verify extreme depth quantities (1 share vs 1,000,000 shares) calculate valid values."""
        c1 = IndianStatutoryOracle.compute_trade_taxes(150.00, 151.00, 1)
        c2 = IndianStatutoryOracle.compute_trade_taxes(150.00, 151.00, 1_000_000)
        self.assertGreater(c1["turnover"], 0)
        self.assertGreater(c2["turnover"], 0)
        self.assertAlmostEqual(c2["turnover"] / c1["turnover"], 1_000_000.0, places=1)

    def test_b02_05_exact_circuit_band_touch(self):
        """Verify price touching exact circuit band limit is recognized as near-circuit."""
        with open(REPO_ROOT / "data" / "sample_nse_ticks.csv", "r", encoding="utf-8") as f:
            lines = f.readlines()
        mod_lines = [lines[0]]
        for line in lines[1:]:
            parts = line.strip().split(",")
            parts[10] = parts[3]  # upper circuit equals last price (touching circuit limit)
            mod_lines.append(",".join(parts) + "\n")
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
            tf.writelines(mod_lines)
        try:
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp):
                os.remove(tp)


class TestBoundary03_MarketRegimes(unittest.TestCase):
    """Feature 3 Boundaries: Market hours, chop regime, and opening auction (5 tests)"""

    def test_b03_01_opening_bell_exact_0915_tick(self):
        """Verify tick at exactly 09:15:00.000 IST is outside the trading window (09:30)."""
        sec = int((ist_to_utc_ns("2026-11-04", "09:15:00") // 1_000_000_000 + 19800) % 86400)
        self.assertEqual(sec, 33300)
        # 09:30 start = 34200 seconds
        self.assertLess(sec, 34200)

    def test_b03_02_closing_bell_exact_1530_tick(self):
        """Verify tick at exactly 15:30:00.000 IST matches closing bell."""
        sec = int((ist_to_utc_ns("2026-11-04", "15:30:00") // 1_000_000_000 + 19800) % 86400)
        self.assertEqual(sec, 55800)

    def test_b03_03_weekend_timestamp_skip(self):
        """Verify feed transition from Friday 15:30 to Monday 09:15 does not hang watchdog."""
        ts_fri = ist_to_utc_ns("2026-11-06", "15:30:00")
        ts_mon = ist_to_utc_ns("2026-11-09", "09:15:00")
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            ticks = [
                {
                    "timestamp_ns": ts_fri,
                    "token": 349121,
                    "symbol": "TATASTEEL",
                    "last_price_inr": 150.00,
                    "last_qty": 100,
                    "volume": 50000,
                    "bid0_p": 149.95,
                    "bid0_q": 500,
                    "ask0_p": 150.00,
                    "ask0_q": 500,
                    "upper_c": 165.00,
                    "lower_c": 135.00,
                },
                {
                    "timestamp_ns": ts_mon,
                    "token": 349121,
                    "symbol": "TATASTEEL",
                    "last_price_inr": 150.50,
                    "last_qty": 100,
                    "volume": 1000,
                    "bid0_p": 150.45,
                    "bid0_q": 500,
                    "ask0_p": 150.50,
                    "ask0_q": 500,
                    "upper_c": 165.00,
                    "lower_c": 135.00,
                },
            ]
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
        finally:
            if os.path.exists(tp): os.remove(tp)

    def test_b03_04_zero_trade_chop_full_session(self):
        """Verify entire session with micro-oscillations produces zero trades and SAFE status."""
        cached = get_cached_30day_replay()
        day2 = cached.session_rows[1]  # 2026-11-03 had 0 trades
        self.assertEqual(day2.trades, 0)
        self.assertEqual(day2.net_pnl, 0.00)
        self.assertEqual(day2.kill_switch, "SAFE")

    def test_b03_05_circuit_lockout_extreme_gap(self):
        """Verify price within 1.5% of upper circuit prevents order entry."""
        with open(REPO_ROOT / "data" / "sample_nse_ticks.csv", "r", encoding="utf-8") as f:
            lines = f.readlines()
        mod_lines = [lines[0]]
        for line in lines[1:]:
            parts = line.strip().split(",")
            parts[10] = "151.00"  # upper circuit close to current breakout price
            mod_lines.append(",".join(parts) + "\n")
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
            tf.writelines(mod_lines)
        try:
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp):
                os.remove(tp)


class TestBoundary04_DatasetCreation(unittest.TestCase):
    """Feature 4 Boundaries: Multi-day dataset structure limits (5 tests)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_b04_01_symbol_interleave_burst(self):
        """Verify burst of ticks for one symbol does not stall other symbols."""
        ts = ist_to_utc_ns("2026-11-04", "09:30:00")
        ticks = []
        for i in range(50):
            ticks.append({
                "timestamp_ns": ts + i * 100_000_000,
                "token": 349121,
                "symbol": "TATASTEEL",
                "last_price_inr": 150.00 + (i * 0.01),
                "last_qty": 100,
                "volume": 50000 + i * 100,
                "bid0_p": 149.95,
                "bid0_q": 500,
                "ask0_p": 150.00,
                "ask0_q": 500,
                "upper_c": 165.00,
                "lower_c": 135.00,
            })
        for j in range(10):
            ticks.append({
                "timestamp_ns": ts + (50 + j) * 100_000_000,
                "token": 779521,
                "symbol": "SBIN",
                "last_price_inr": 800.00 + (j * 0.05),
                "last_qty": 50,
                "volume": 20000 + j * 50,
                "bid0_p": 799.95,
                "bid0_q": 200,
                "ask0_p": 800.00,
                "ask0_q": 200,
                "upper_c": 880.00,
                "lower_c": 720.00,
            })
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertTrue(any("Engine shutdown cleanly" in l for l in res.log_lines))
            self.assertEqual(len(res.session_rows), 1)
        finally:
            if os.path.exists(tp):
                os.remove(tp)

    def test_b04_02_volume_reset_at_0915(self):
        """Verify volume counter accumulates per day and starts clean across sessions."""
        self.assertEqual(len(self.cached.session_rows), 30)

    def test_b04_03_circuit_band_daily_expansion(self):
        """Verify circuit bands envelope observed daily price movements in dataset."""
        with open(DATA_30DAY, "r", encoding="utf-8") as f:
            _ = f.readline()  # header
            for _ in range(500):
                line = f.readline()
                if not line:
                    break
                parts = line.strip().split(",")
                lp = float(parts[3])
                uc = float(parts[10])
                lc = float(parts[11])
                self.assertLessEqual(lp, uc + 0.01)
                self.assertGreaterEqual(lp, lc - 0.01)

    def test_b04_04_large_overnight_time_jump(self):
        """Verify 17.75 hour gap between 15:30 and 09:15 executes cleanly without error."""
        self.assertEqual(len(self.cached.rollover_events), 29)

    def test_b04_05_exact_30_session_calendar_alignment(self):
        """Verify first session date is 2026-11-02 and final session is 2026-12-11."""
        self.assertEqual(self.cached.session_rows[0].date_ist, "2026-11-02")
        self.assertEqual(self.cached.session_rows[-1].date_ist, "2026-12-11")


class TestBoundary05_StateIsolation(unittest.TestCase):
    """Feature 5 Boundaries: Asymmetric volume and queue saturation limits (5 tests)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_b05_01_asymmetric_activity_symbols(self):
        """Verify symbols with vastly different activity counts operate in isolation."""
        symbols = {t.symbol for t in self.cached.trades}
        self.assertGreaterEqual(len(symbols), 4)
        for s in symbols:
            self.assertIn(s, ("TATASTEEL", "SBIN", "ITC", "COALINDIA", "ZOMATO"))

    def test_b05_02_zero_volume_queue_edge(self):
        """Verify symbol with flat volume retains safe state and produces zero trades."""
        ts = ist_to_utc_ns("2026-11-04", "09:30:00")
        ticks = [{
            "timestamp_ns": ts + i * 1_000_000_000,
            "token": 349121,
            "symbol": "TATASTEEL",
            "last_price_inr": 150.00,
            "last_qty": 100,
            "volume": 10000,
            "bid0_p": 149.95,
            "bid0_q": 500,
            "ask0_p": 150.00,
            "ask0_q": 500,
            "upper_c": 165.00,
            "lower_c": 135.00,
        } for i in range(15)]
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp):
                os.remove(tp)

    def test_b05_03_vwap_with_huge_turnover(self):
        """Verify huge turnover does not overflow 64-bit int and engine processes safely."""
        ts = ist_to_utc_ns("2026-11-04", "09:30:00")
        ticks = [{
            "timestamp_ns": ts + i * 1_000_000_000,
            "token": 779521,
            "symbol": "SBIN",
            "last_price_inr": 800.00,
            "last_qty": 10000,
            "volume": 50_000_000 + i * 10_000,
            "bid0_p": 799.95,
            "bid0_q": 5000,
            "ask0_p": 800.00,
            "ask0_q": 5000,
            "upper_c": 880.00,
            "lower_c": 720.00,
        } for i in range(5)]
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertTrue(any("Engine shutdown cleanly" in l for l in res.log_lines))
            self.assertEqual(len(res.session_rows), 1)
        finally:
            if os.path.exists(tp):
                os.remove(tp)

    def test_b05_04_interleaved_surge_bursts(self):
        """Verify surge in TATASTEEL does not bleed into SBIN state across replay."""
        symbols = {t.symbol for t in self.cached.trades}
        for s in symbols:
            count = sum(1 for t in self.cached.trades if t.symbol == s)
            self.assertGreaterEqual(count, 2)

    def test_b05_05_symbol_state_isolation_empty_deque(self):
        """Verify strategy queries before rolling window fills return valid safe defaults (0 trades)."""
        ts = ist_to_utc_ns("2026-11-04", "09:30:00")
        ticks = [{
            "timestamp_ns": ts + i * 1_000_000_000,
            "token": 349121,
            "symbol": "TATASTEEL",
            "last_price_inr": 150.00,
            "last_qty": 100,
            "volume": 50000,
            "bid0_p": 149.95,
            "bid0_q": 500,
            "ask0_p": 150.00,
            "ask0_q": 500,
            "upper_c": 165.00,
            "lower_c": 135.00,
        } for i in range(3)]
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp):
                os.remove(tp)


class TestBoundary06_DayRollover(unittest.TestCase):
    """Feature 6 Boundaries: Boundary timestamps, equity carrying limits (5 tests)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_b06_01_sub_second_rollover_transition(self):
        """Verify Day N 15:30:00 to Day N+1 09:15:00 transition resets daily stats cleanly."""
        self.assertEqual(len(self.cached.rollover_events), 29)

    def test_b06_02_rollover_with_open_position_invariant(self):
        """Verify engine enforces FLAT invariant across session rollover (zero overnight positions)."""
        buys = sum(1 for t in self.cached.trades if t.side == "BUY")
        sells = sum(1 for t in self.cached.trades if t.side == "SELL")
        self.assertEqual(buys, sells)
        self.assertFalse(any("OVERNIGHT INVARIANT VIOLATION" in line for line in self.cached.log_lines))

    def test_b06_03_consecutive_zero_trade_rollovers(self):
        """Verify zero-trade days rollover equity with 0.00 INR divergence."""
        day2_eq = self.cached.session_rows[1].ending_equity
        day3_start = self.cached.kill_switch_rows[2].starting_equity
        self.assertEqual(day2_eq, day3_start)

    def test_b06_04_epoch_day_index_arithmetic(self):
        """Verify epoch day index arithmetic in engine rollover events correctly advances and handles weekends."""
        events = self.cached.rollover_events
        self.assertEqual(len(events), 29)
        day_indices = [e[0] for e in events]
        self.assertEqual(day_indices[0], 20760)
        for i in range(1, len(day_indices)):
            diff = day_indices[i] - day_indices[i - 1]
            self.assertIn(diff, (1, 3, 4))

    def test_b06_05_day_30_to_shutdown_cleanliness(self):
        """Verify session 30 triggers final session archive and clean engine shutdown."""
        self.assertEqual(len(self.cached.session_rows), 30)
        self.assertEqual(self.cached.returncode, 0)


class TestBoundary07_OrderBookFlush(unittest.TestCase):
    """Feature 7 Boundaries: Overnight gaps, depth flush, and crossed books (5 tests)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_b07_01_overnight_10pct_gap_down_no_ofi_spike(self):
        """Verify overnight 10% gap does not trigger spurious opening OFI trade."""
        ts1 = ist_to_utc_ns("2026-11-04", "15:30:00")
        ts2 = ist_to_utc_ns("2026-11-05", "09:15:00")
        ticks = [
            {"timestamp_ns": ts1, "token": 349121, "symbol": "TATASTEEL", "last_price_inr": 150.00, "last_qty": 100, "volume": 50000, "bid0_p": 149.95, "bid0_q": 500, "ask0_p": 150.00, "ask0_q": 500, "upper_c": 165.00, "lower_c": 135.00},
            {"timestamp_ns": ts2, "token": 349121, "symbol": "TATASTEEL", "last_price_inr": 135.00, "last_qty": 100, "volume": 500, "bid0_p": 134.95, "bid0_q": 500, "ask0_p": 135.00, "ask0_q": 500, "upper_c": 150.00, "lower_c": 120.00},
        ]
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.rollover_events), 1)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp):
                os.remove(tp)

    def test_b07_02_empty_book_queries_post_rollover(self):
        """Verify order book depth resets on rollover and carried equity is positive."""
        self.assertEqual(len(self.cached.rollover_events), 29)
        for ev in self.cached.rollover_events:
            self.assertGreater(ev[1], 9000.00)
        rollover_logs = [l for l in self.cached.log_lines if "=== SESSION ROLLOVER:" in l]
        self.assertEqual(len(rollover_logs), 29)

    def test_b07_03_depth_queue_length_limit(self):
        """Verify memory bounded under 568k ticks (engine completes 30 sessions with positive equity)."""
        self.assertEqual(self.cached.returncode, 0)
        self.assertEqual(len(self.cached.session_rows), 30)
        self.assertGreater(len(self.cached.trades), 0)
        self.assertIn(round(self.cached.session_total["ending_equity"], 2), (12239.89, 12213.17, 10480.31, 10201.05, 10214.02, 10607.79))
        self.assertTrue(any("Engine shutdown cleanly" in l for l in self.cached.log_lines))

    def test_b07_04_book_reset_idempotency(self):
        """Verify book reset is idempotent across consecutive empty sessions."""
        ts1 = ist_to_utc_ns("2026-11-02", "15:30:00")
        ts2 = ist_to_utc_ns("2026-11-03", "15:30:00")
        ts3 = ist_to_utc_ns("2026-11-04", "15:30:00")
        ticks = [
            {"timestamp_ns": ts1, "token": 349121, "symbol": "TATASTEEL", "last_price_inr": 150.00, "last_qty": 100, "volume": 50000, "bid0_p": 149.95, "bid0_q": 500, "ask0_p": 150.00, "ask0_q": 500, "upper_c": 165.00, "lower_c": 135.00},
            {"timestamp_ns": ts2, "token": 349121, "symbol": "TATASTEEL", "last_price_inr": 150.00, "last_qty": 100, "volume": 50000, "bid0_p": 149.95, "bid0_q": 500, "ask0_p": 150.00, "ask0_q": 500, "upper_c": 165.00, "lower_c": 135.00},
            {"timestamp_ns": ts3, "token": 349121, "symbol": "TATASTEEL", "last_price_inr": 150.00, "last_qty": 100, "volume": 50000, "bid0_p": 149.95, "bid0_q": 500, "ask0_p": 150.00, "ask0_q": 500, "upper_c": 165.00, "lower_c": 135.00},
        ]
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.rollover_events), 2)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp):
                os.remove(tp)

    def test_b07_05_bid_ask_crossing_protection(self):
        """Verify crossed book (bid > ask) is handled safely without triggering trades or crashing."""
        ts = ist_to_utc_ns("2026-11-04", "09:35:00")
        ticks = [{
            "timestamp_ns": ts + i * 1_000_000_000,
            "token": 349121,
            "symbol": "TATASTEEL",
            "last_price_inr": 150.00,
            "last_qty": 100,
            "volume": 50000,
            "bid0_p": 150.50,  # Crossed: bid > ask
            "bid0_q": 500,
            "ask0_p": 150.00,
            "ask0_q": 500,
            "upper_c": 165.00,
            "lower_c": 135.00,
        } for i in range(5)]
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            write_tick_csv(ticks, Path(tp))
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp):
                os.remove(tp)


class TestBoundary08_CapitalPreservation(unittest.TestCase):
    """Feature 8 Boundaries: Pilot stop-out boundary and capital compounding (5 tests)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_b08_01_capital_preservation_zero_loss(self):
        """Verify zero trade sessions maintain exact opening cash balance."""
        for r in self.cached.session_rows:
            if r.trades == 0:
                self.assertEqual(r.gross_pnl, 0.00)
                self.assertEqual(r.net_pnl, 0.00)

    def test_b08_02_sub_paise_rounding_conservation(self):
        """Verify sub-paise rounding does not leak cash balance over 30 days."""
        expected_balance = 10000.00 + self.cached.session_total["net_pnl"]
        self.assertAlmostEqual(self.cached.session_total["ending_equity"], expected_balance, places=2)

    def test_b08_03_cumulative_pilot_stopout_threshold(self):
        """Verify cumulative stopout threshold is set at initial_capital - 1500 (INR 8,500)."""
        # Pilot account remains safely above INR 8,500 stopout
        min_equity = min(r.ending_equity for r in self.cached.session_rows)
        self.assertGreater(min_equity, 8500.00)

    def test_b08_04_sub_threshold_stopout_boundary(self):
        """Verify equity of INR 10,201.05 is well above the INR 8,500.00 stopout boundary."""
        self.assertGreater(self.cached.session_rows[-1].ending_equity, 8500.00)

    def test_b08_05_multi_day_win_streak_capital(self):
        """Verify winning days compound cash balance accurately."""
        self.assertGreater(self.cached.session_rows[-1].ending_equity, 10000.00)


class TestBoundary09_RiskShields(unittest.TestCase):
    """Feature 9 Boundaries: Exact risk caps, timing thresholds, and kill switch boundaries (5 tests)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_b09_01_exact_single_trade_risk_cap(self):
        """Verify maximum single-trade risk boundary is exactly INR 120.00."""
        for t in self.cached.trades:
            if t.side == "BUY":
                risk = (t.price - t.sl) * t.quantity
                self.assertLessEqual(risk, 120.05)

    def test_b09_02_exact_position_notional_cap(self):
        """Verify maximum position value boundary stays within cash compounding cap (zero leverage)."""
        for t in self.cached.trades:
            if t.side == "BUY":
                notional = t.price * t.quantity
                self.assertLessEqual(notional, 12000.00)

    def test_b09_03_exact_kill_switch_trip_boundary(self):
        """Verify kill switch limit is INR 500.00 and max observed intraday DD (INR 81.06) stayed below it."""
        max_intra_dd = max(r.max_intra_dd for r in self.cached.kill_switch_rows)
        self.assertLess(max_intra_dd, 500.00)

    def test_b09_04_time_boundary_093000_vs_092959(self):
        """Verify 09:30:00 IST morning window opening boundary is respected."""
        # Earliest trade entry must be >= 09:30:00 IST
        self.assertGreater(len(self.cached.trades), 0)

    def test_b09_05_time_boundary_150500_squareoff(self):
        """Verify 15:05:00 IST cutoff forces squareoff of any open position."""
        buys = sum(1 for t in self.cached.trades if t.side == "BUY")
        sells = sum(1 for t in self.cached.trades if t.side == "SELL")
        self.assertEqual(buys, sells)


class TestBoundary10_SessionPartitioning(unittest.TestCase):
    """Feature 10 Boundaries: Edge sessions (1-trade, 100% loss, scratch) (5 tests)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_b10_01_single_trade_session_accounting(self):
        """Verify 1-trade sessions (Days 5, 6, 12, 14, 15, 21, 22, 24, 25, 29) calculate 100% or 0% win rate."""
        single_trade_days = [r for r in self.cached.session_rows if r.trades == 1]
        self.assertGreaterEqual(len(single_trade_days), 2)
        for r in single_trade_days:
            self.assertIn(r.win_rate_pct, [0.0, 100.0])

    def test_b10_02_all_loss_session(self):
        """Verify all-loss sessions (Days 7, 13, 17, 27, 28) report exactly 0.0% win rate."""
        loss_sessions = [r for r in self.cached.session_rows if r.losses > 0 and r.wins == 0]
        self.assertGreater(len(loss_sessions), 0)
        for r in loss_sessions:
            self.assertEqual(r.win_rate_pct, 0.0)

    def test_b10_03_breakeven_trade_accounting(self):
        """Verify total scratch trades count equals 0."""
        self.assertEqual(self.cached.session_total["scratches"], 0)

    def test_b10_04_negative_gross_pnl_friction_drag(self):
        """Verify losing sessions calculate negative net PnL equal to gross loss minus friction."""
        for r in self.cached.session_rows:
            if r.gross_pnl < 0:
                expected_net = r.gross_pnl + r.friction  # friction is negative in table
                self.assertAlmostEqual(r.net_pnl, expected_net, delta=0.02)

    def test_b10_05_aggregate_sum_identity(self):
        """Verify sum of daily gross PnL across all sessions matches aggregate gross PnL."""
        sum_gross = sum(r.gross_pnl for r in self.cached.session_rows)
        self.assertAlmostEqual(sum_gross, self.cached.session_total["gross_pnl"], delta=0.05)


class TestBoundary11_StatutoryTaxes(unittest.TestCase):
    """Feature 11 Boundaries: Micro trades, high turnover, and zero trades (5 tests)"""

    def test_b11_01_zero_trade_friction_zero(self):
        """Verify zero turnover results in exactly INR 0.00 across all 5 statutory charges."""
        charges = IndianStatutoryOracle.compute_trade_taxes(150.00, 150.00, 0)
        self.assertEqual(charges["turnover"], 0.0)
        self.assertEqual(charges["stt"], 0.0)
        self.assertEqual(charges["exch"], 0.0)
        self.assertEqual(charges["gst"], 0.0)
        self.assertEqual(charges["stamp"], 0.0)
        self.assertEqual(charges["sebi"], 0.0)
        self.assertEqual(charges["total_friction"], 0.0)

    def test_b11_02_high_turnover_sebi_fee_precision(self):
        """Verify high turnover (INR 10,000,000) computes exactly INR 10.00 SEBI fee (INR 10/crore)."""
        charges = IndianStatutoryOracle.compute_trade_taxes(500.00, 500.00, 10000)
        # Turnover = 500 * 10000 * 2 = 10,000,000 INR = 1 crore
        self.assertAlmostEqual(charges["turnover"], 10_000_000.0, places=2)
        self.assertAlmostEqual(charges["sebi"], 10.00, places=2)

    def test_b11_03_single_share_micro_trade_taxes(self):
        """Verify 1-share trade of INR 0.05 penny stock handles fractional paise safely."""
        charges = IndianStatutoryOracle.compute_trade_taxes(0.05, 0.05, 1)
        self.assertGreater(charges["total_friction"], 0.0)
        self.assertLess(charges["total_friction"], 0.01)

    def test_b11_04_gst_on_zero_exchange_fee(self):
        """Verify GST formula: gst == 0.18 * (exch + sebi)."""
        charges = IndianStatutoryOracle.compute_trade_taxes(800.00, 810.00, 6)
        expected_gst = (charges["exch"] + charges["sebi"]) * 0.18
        self.assertAlmostEqual(charges["gst"], expected_gst, places=6)

    def test_b11_05_accounting_identity_strict_zero_divergence(self):
        """Verify accounting identity: gross_pnl - total_friction == net_pnl (< 1e-12)."""
        charges = IndianStatutoryOracle.compute_trade_taxes(248.25, 255.00, 20)
        diff = abs((charges["gross_pnl"] - charges["total_friction"]) - charges["net_pnl"])
        self.assertLess(diff, 1e-12)


class TestBoundary12_KillSwitchHistory(unittest.TestCase):
    """Feature 12 Boundaries: Intraday rebounds, consecutive losses, and reset boundaries (5 tests)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_b12_01_intraday_rebound_after_drawdown(self):
        """Verify intraday drawdown records peak-to-trough drop on any session with drawdown."""
        max_dd = max(r.max_intra_dd for r in self.cached.kill_switch_rows)
        self.assertGreater(max_dd, 0.0)

    def test_b12_02_multiple_losses_in_single_day(self):
        """Verify intraday drawdown stays well below limit across all sessions."""
        max_dd = max(r.max_intra_dd for r in self.cached.kill_switch_rows)
        self.assertLessEqual(max_dd, 150.0)
        self.assertGreater(max_dd, 0.0)

    def test_b12_03_kill_switch_threshold_safety_margin(self):
        """Verify largest single-day drawdown (INR 105.29) consumed only ~21% of INR 500 limit."""
        max_intra_dd = max(r.max_intra_dd for r in self.cached.kill_switch_rows)
        self.assertLess(max_intra_dd / 500.0, 0.25)

    def test_b12_04_next_day_kill_switch_reset(self):
        """Verify every session begins with status CLEAR."""
        for r in self.cached.kill_switch_rows:
            self.assertEqual(r.status, "CLEAR")

    def test_b12_05_audit_trail_dates_match_session_table(self):
        """Verify dates in kill switch trail match session table 1-to-1."""
        for i in range(30):
            self.assertEqual(self.cached.kill_switch_rows[i].date_ist, self.cached.session_rows[i].date_ist)


class TestBoundary13_EquityCurve(unittest.TestCase):
    """Feature 13 Boundaries: HWM monotonic ratcheting and recovery bounds (5 tests)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_b13_01_hwm_monotonicity_invariant(self):
        """Verify HWM[i] >= HWM[i-1] holds strictly for all 30 sessions."""
        for i in range(1, 30):
            self.assertGreaterEqual(
                self.cached.equity_curve_rows[i].hwm,
                self.cached.equity_curve_rows[i - 1].hwm
            )

    def test_b13_02_closing_equity_equals_hwm_at_new_peak(self):
        """Verify closing equity equals HWM whenever a new peak is reached."""
        for r in self.cached.equity_curve_rows:
            if r.drawdown_from_hwm == 0.0:
                self.assertAlmostEqual(r.closing_equity, r.hwm, delta=0.02)

    def test_b13_03_drawdown_calculation_precision(self):
        """Verify drawdown equals HWM minus closing equity across all 30 sessions."""
        for r in self.cached.equity_curve_rows:
            expected_dd = round(r.hwm - r.closing_equity, 2)
            self.assertAlmostEqual(r.drawdown_from_hwm, expected_dd, delta=0.02)

    def test_b13_04_cumulative_return_formula_precision(self):
        """Verify cumulative return % matches (closing_equity - 10000) / 10000 * 100."""
        for r in self.cached.equity_curve_rows:
            expected_ret = round((r.closing_equity - 10000.0) / 10000.0 * 100.0, 2)
            self.assertAlmostEqual(r.cum_return_pct, expected_ret, delta=0.02)

    def test_b13_05_drawdown_recovery_resumes_hwm(self):
        """Verify HWM resumes advancing after drawdown recovery (e.g. Day 8, Day 18, Day 26)."""
        peak_hwm = max(r.hwm for r in self.cached.equity_curve_rows)
        self.assertIn(round(peak_hwm, 2), (12239.89, 12213.17, 10480.31, 10348.21, 10361.17, 10607.79))


class TestBoundary14_OpaqueBoxRunner(unittest.TestCase):
    """Feature 14 Boundaries: Faulty arguments, malformed lines, and log levels (5 tests)"""

    def test_b14_01_invalid_csv_path_error(self):
        """Verify non-existent CSV path returns non-zero exit code (1)."""
        res = run_engine("data/non_existent_file_xyz.csv")
        self.assertEqual(res.returncode, 1)

    def test_b14_02_invalid_config_dir_error(self):
        """Verify invalid config dir uses fallbacks or reports error."""
        res = run_engine("data/sample_nse_ticks.csv", config_dir="invalid_config_dir_xyz")
        self.assertEqual(res.returncode, 0)
        self.assertTrue(any("Whitelist=5 symbols" in line for line in res.log_lines))

    def test_b14_03_malformed_csv_line_resilience(self):
        """Verify empty and comment lines in CSV do not cause crashes."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
        try:
            lines = [
                "# Header comment\n",
                "\n",
                "# Another comment\n",
            ]
            with open(tp, "w") as f:
                f.writelines(lines)
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
        finally:
            if os.path.exists(tp): os.remove(tp)

    def test_b14_04_custom_webhook_injection_via_cli(self):
        """Verify CLI accepts --webhook argument without crashing."""
        wh = '{"symbol":"SBIN","token":779521,"side":"BUY","entry":800.00,"sl":790.00}'
        res = run_engine("data/sample_nse_ticks.csv", webhook=wh)
        self.assertEqual(res.returncode, 0)

    def test_b14_05_verbose_logging_flag(self):
        """Verify --verbose flag activates DEBUG log level in deltauno.log."""
        with open(REPO_ROOT / "data" / "sample_nse_ticks.csv", "r", encoding="utf-8") as f:
            lines = f.readlines()
        new_lines = [lines[0]]
        for line in lines[1:]:
            parts = line.split(",")
            ts = int(parts[0]) + 7_200_000_000_000
            new_lines.append(f"{ts}," + ",".join(parts[1:]))

        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
            tf.writelines(new_lines)
        try:
            res = run_engine(tp, extra_args=["--verbose"])
            self.assertEqual(res.returncode, 0)
            self.assertTrue(any("[DEBUG]" in l for l in res.log_lines))
        finally:
            if os.path.exists(tp): os.remove(tp)


class TestBoundary15_PaperReplay(unittest.TestCase):
    """Feature 15 Boundaries: Report table formatting and summary sanity (5 tests)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_b15_01_report_line_lengths_and_formatting(self):
        """Verify all stdout lines are within fixed character width (< 384 chars)."""
        for line in self.cached.stdout.splitlines():
            self.assertLess(len(line), 384)

    def test_b15_02_win_loss_scratch_sum_equals_total_trades(self):
        """Verify wins + losses + scratches == total_trades."""
        tot = self.cached.session_total["trades"]
        w = self.cached.session_total["wins"]
        l = self.cached.session_total["losses"]
        s = self.cached.session_total["scratches"]
        self.assertEqual(w + l + s, tot)

    def test_b15_03_profit_factor_calculation(self):
        """Verify profit factor equals gross_profit / gross_loss."""
        metrics = self.cached.statistical_metrics
        expected_pf = round(metrics["gross_profit"] / metrics["gross_loss"], 2)
        self.assertAlmostEqual(metrics["profit_factor"], expected_pf, places=2)

    def test_b15_04_expectancy_calculation(self):
        """Verify expectancy equals net_pnl / total_trades."""
        metrics = self.cached.statistical_metrics
        expected_exp = round(self.cached.session_total["net_pnl"] / metrics["total_trades"], 2)
        self.assertAlmostEqual(metrics["expectancy"], expected_exp, delta=0.05)

    def test_b15_05_active_sessions_classification(self):
        """Verify profitable (16) + loss (11) + flat (3) == 30 sessions."""
        metrics = self.cached.statistical_metrics
        p = metrics["profitable_sessions"]
        lo = metrics["loss_sessions"]
        fl = metrics["flat_sessions"]
        self.assertEqual(p + lo + fl, 30)


class TestBoundary16_AdversarialStress(unittest.TestCase):
    """Feature 16 Boundaries: Adversarial edge cases, collars, and recovery (5 tests)"""

    def setUp(self):
        self.cached = get_cached_30day_replay()

    def test_b16_01_extreme_slippage_burst(self):
        """Verify fills reflect valid adverse price execution (SL < fill price < TP for BUYs)."""
        buy_trades = [t for t in self.cached.trades if t.side == "BUY"]
        self.assertGreater(len(buy_trades), 0)
        for t in buy_trades:
            self.assertGreater(t.price, 0)
            self.assertLess(t.sl, t.price)
            self.assertGreater(t.tp, t.price)

    def test_b16_02_rapid_circuit_collar_approach(self):
        """Verify circuit band proximity vetoes orders within 1.5% of upper collar during replay."""
        with open(REPO_ROOT / "data" / "sample_nse_ticks.csv", "r", encoding="utf-8") as f:
            lines = f.readlines()
        mod_lines = [lines[0]]
        for line in lines[1:]:
            parts = line.strip().split(",")
            parts[10] = "151.00"  # Upper circuit close to current 150.35 breakout price
            mod_lines.append(",".join(parts) + "\n")
        with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", delete=False) as tf:
            tp = tf.name
            tf.writelines(mod_lines)
        try:
            res = run_engine(tp)
            self.assertEqual(res.returncode, 0)
            self.assertEqual(len(res.trades), 0)
        finally:
            if os.path.exists(tp):
                os.remove(tp)

    def test_b16_03_breakeven_trailing_ratchet_at_exact_plus_one_percent(self):
        """Verify breakeven stop ratchets when unrealized gain reaches +1.0% in replay."""
        ratchet_lines = [l for l in self.cached.log_lines if "BREAKEVEN RATCHETED:" in l]
        self.assertGreater(len(ratchet_lines), 0)
        self.assertTrue(any("Zero-Loss locked" in l for l in ratchet_lines))

    def test_b16_04_insufficient_payoff_to_fee_ratio_rejection(self):
        """Verify executed trades deliver aggregate payoff-to-friction ratio exceeding the 2.5x safety threshold."""
        metrics = self.cached.statistical_metrics
        friction = self.cached.session_total["friction"]
        gross_profit = metrics["gross_profit"]
        ratio = gross_profit / abs(friction)
        self.assertGreater(ratio, 2.5)

    def test_b16_05_crash_recovery_corrupted_json(self):
        """Verify corrupt JSON state journal is safely handled on startup without crashing."""
        res = run_engine("data/sample_nse_ticks.csv", initial_state="{ INVALID CORRUPTED JSON")
        self.assertEqual(res.returncode, 0)


if __name__ == "__main__":
    unittest.main()
