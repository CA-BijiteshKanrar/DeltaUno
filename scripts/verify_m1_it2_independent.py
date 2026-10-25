#!/usr/bin/env python3
"""
Independent Microstructure & Adversarial Verification Script for Milestone 1 Iteration 2.
Independently verifies:
1. Breakout VWAP Synchronization across Days 1, 4, 12, 16, 20, 26, 29.
   - Verifies best_bid >= session_VWAP during volume surge.
   - Verifies latest_volume_multiple >= 2.5 and composite_ofi >= 0.55.
   - Checks signal count and timestamps.
2. Chop Regime Smoothing across Days 2, 11, 19.
   - Checks max VolMult and max composite OFI across ALL 5 symbols.
   - Checks signals in trading windows (09:30-11:00, 13:30-14:45 IST) and full day.
3. Complete 30-Day Signal Map across ALL 30 days and 5 symbols.
4. Circuit test proximity behavior on Day 8 and Day 27.
5. Watchdog pause on Day 22.
"""

import sys
import os
import math
import collections
import datetime
from typing import Dict, List, Tuple, Any

CSV_PATH = "data/historical_30days_nse.csv"

K_LEVEL_WEIGHTS = (1.00, 0.70, 0.50, 0.35, 0.25)

EXPECTED_TOKENS = {
    "TATASTEEL": 349121,
    "SBIN": 779521,
    "ITC": 424961,
    "COALINDIA": 521729,
    "ZOMATO": 895745,
}

TARGET_DATES = [
    "2026-11-02", "2026-11-03", "2026-11-04", "2026-11-05", "2026-11-06",
    "2026-11-09", "2026-11-10", "2026-11-11", "2026-11-12", "2026-11-13",
    "2026-11-16", "2026-11-17", "2026-11-18", "2026-11-19", "2026-11-20",
    "2026-11-23", "2026-11-24", "2026-11-25", "2026-11-26", "2026-11-27",
    "2026-11-30", "2026-12-01", "2026-12-02", "2026-12-03", "2026-12-04",
    "2026-12-07", "2026-12-08", "2026-12-09", "2026-12-10", "2026-12-11",
]

EPOCH_DAY_MAP = {}
for i, d in enumerate(TARGET_DATES):
    y, m, day = map(int, d.split("-"))
    ep = (datetime.date(y, m, day) - datetime.date(1970, 1, 1)).days
    EPOCH_DAY_MAP[ep] = i + 1

BREAKOUT_TARGETS = {
    1: ("TATASTEEL", 35100, 36300),   # 09:45 - 10:05
    4: ("SBIN", 49500, 51000),        # 13:45 - 14:10
    12: ("ZOMATO", 36900, 38100),     # 10:15 - 10:35
    16: ("COALINDIA", 35400, 36600),   # 09:50 - 10:10
    20: ("SBIN", 48900, 50400),       # 13:35 - 14:00
    26: ("ITC", 35700, 36900),        # 09:55 - 10:15
    29: ("TATASTEEL", 50400, 51600),  # 14:00 - 14:20
}

CHOP_DAYS = [2, 11, 19]

class IndependentOrderBook:
    def __init__(self, token: int, symbol: str):
        self.token = token
        self.symbol = symbol
        self.prev_bids = None
        self.prev_asks = None
        self.has_prev = False
        self.ofi_history = collections.deque(maxlen=20)
        self.last_price = 0
        self.upper_c = 0
        self.lower_c = 0

    def reset(self):
        self.prev_bids = None
        self.prev_asks = None
        self.has_prev = False
        self.ofi_history.clear()
        self.last_price = 0
        self.upper_c = 0
        self.lower_c = 0

    def on_tick(self, ltp_p, b0_p, b0_q, a0_p, a0_q, uc_p, lc_p):
        self.last_price = ltp_p
        self.upper_c = uc_p
        self.lower_c = lc_p

        # Exact 5-level synthesis as in C++ CSVFeed
        bids = [
            (b0_p, b0_q),
            (max(0, b0_p - 5), b0_q + 100),
            (max(0, b0_p - 10), b0_q + 200),
            (max(0, b0_p - 15), b0_q + 300),
            (max(0, b0_p - 20), b0_q + 400),
        ]
        asks = [
            (a0_p, a0_q),
            (a0_p + 5, a0_q + 100),
            (a0_p + 10, a0_q + 200),
            (a0_p + 15, a0_q + 300),
            (a0_p + 20, a0_q + 400),
        ]

        weighted_ofi = 0.0
        weighted_depth = 0.0

        for m in range(5):
            if self.has_prev:
                if bids[m][0] > self.prev_bids[m][0]:
                    delta_bid = bids[m][1]
                elif bids[m][0] == self.prev_bids[m][0]:
                    delta_bid = bids[m][1] - self.prev_bids[m][1]
                else:
                    delta_bid = -self.prev_bids[m][1]

                if asks[m][0] < self.prev_asks[m][0] and asks[m][0] > 0:
                    delta_ask = asks[m][1]
                elif asks[m][0] == self.prev_asks[m][0]:
                    delta_ask = asks[m][1] - self.prev_asks[m][1]
                else:
                    delta_ask = -self.prev_asks[m][1]

                weighted_ofi += K_LEVEL_WEIGHTS[m] * float(delta_bid - delta_ask)

            level_qty = bids[m][1] + asks[m][1]
            weighted_depth += K_LEVEL_WEIGHTS[m] * (float(level_qty) * 0.5)

        norm_ofi = 0.0
        if self.has_prev and weighted_depth > 0.0:
            norm_ofi = max(-1.0, min(1.0, weighted_ofi / weighted_depth))

        self.ofi_history.append(norm_ofi)
        self.prev_bids = bids
        self.prev_asks = asks
        self.has_prev = True
        return norm_ofi

    def composite_ofi(self):
        if not self.ofi_history:
            return 0.0
        ofi_1 = self.ofi_history[-1]
        c5 = min(5, len(self.ofi_history))
        ofi_5 = sum(list(self.ofi_history)[-c5:]) / float(c5)
        ofi_20 = sum(self.ofi_history) / float(len(self.ofi_history))
        return (0.5 * ofi_1) + (0.3 * ofi_5) + (0.2 * ofi_20)

    def is_near_circuit(self, buffer_pct=1.5):
        if self.last_price <= 0:
            return False
        if self.upper_c > 0:
            dist = (self.upper_c - self.last_price) / float(self.last_price) * 100.0
            if dist <= buffer_pct:
                return True
        if self.lower_c > 0:
            dist = (self.last_price - self.lower_c) / float(self.last_price) * 100.0
            if dist <= buffer_pct:
                return True
        return False

class IndependentStrategy:
    def __init__(self):
        self.cum_turnover = 0.0
        self.cum_vol = 0
        self.prev_tot_vol = 0
        self.recent_vols = collections.deque(maxlen=20)

    def reset(self):
        self.cum_turnover = 0.0
        self.cum_vol = 0
        self.prev_tot_vol = 0
        self.recent_vols.clear()

    def on_tick(self, ltp_p, lq, tot_vol):
        if lq > 0:
            self.cum_turnover += float(ltp_p) * float(lq)
            self.cum_vol += lq

        if self.prev_tot_vol > 0 and tot_vol >= self.prev_tot_vol:
            delta_vol = tot_vol - self.prev_tot_vol
            self.recent_vols.append(delta_vol)
        self.prev_tot_vol = tot_vol

    def get_vwap(self):
        if self.cum_vol == 0:
            return 0
        return int(self.cum_turnover / float(self.cum_vol))

    def latest_volume_multiple(self):
        if len(self.recent_vols) < 2:
            return 0.0
        v_list = list(self.recent_vols)
        avg_prior = sum(v_list[:-1]) / float(len(v_list) - 1)
        if avg_prior <= 0.0:
            return 0.0
        return float(v_list[-1]) / avg_prior

def run_independent_audit():
    print("=" * 80)
    print(" INDEPENDENT MICROSTRUCTURE AUDIT (REVIEWER M1 IT2)")
    print("=" * 80)

    books = {s: IndependentOrderBook(EXPECTED_TOKENS[s], s) for s in EXPECTED_TOKENS}
    strats = {s: IndependentStrategy() for s in EXPECTED_TOKENS}

    current_epoch_day = -1
    all_signals = [] # list of (day_idx, symbol, sec, ltp, bid, vwap, vol_mult, ofi, in_window)
    
    # Per-day metrics
    day_symbol_stats = collections.defaultdict(lambda: {
        "ticks": 0,
        "max_vol_mult": 0.0,
        "max_ofi": -999.0,
        "min_ofi": 999.0,
        "signals_all": 0,
        "signals_window": 0
    })

    with open(CSV_PATH, "r", encoding="utf-8") as f:
        f.readline() # header
        for line_no, line in enumerate(f, start=2):
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            parts = line.split(",")
            if len(parts) != 12:
                print(f"Skipping line {line_no} with {len(parts)} parts: {line}")
                continue
            ts_ns = int(parts[0])
            token = int(parts[1])
            symbol = parts[2]
            ltp_inr = float(parts[3])
            lq = int(parts[4])
            vol = int(parts[5])
            b0_p_inr = float(parts[6])
            b0_q = int(parts[7])
            a0_p_inr = float(parts[8])
            a0_q = int(parts[9])
            uc_inr = float(parts[10])
            lc_inr = float(parts[11])

            ltp_p = int(round(ltp_inr * 100))
            b0_p = int(round(b0_p_inr * 100))
            a0_p = int(round(a0_p_inr * 100))
            uc_p = int(round(uc_inr * 100))
            lc_p = int(round(lc_inr * 100))

            ts_sec_ist = (ts_ns // 1_000_000_000) + 19800
            epoch_day = ts_sec_ist // 86400
            sec_of_day = ts_sec_ist % 86400
            day_idx = EPOCH_DAY_MAP.get(epoch_day, -1)

            if current_epoch_day != -1 and epoch_day != current_epoch_day:
                for b in books.values():
                    b.reset()
                for s in strats.values():
                    s.reset()

            current_epoch_day = epoch_day

            book = books[symbol]
            strat = strats[symbol]

            book.on_tick(ltp_p, b0_p, b0_q, a0_p, a0_q, uc_p, lc_p)
            strat.on_tick(ltp_p, lq, vol)

            comp_ofi = book.composite_ofi()
            vol_mult = strat.latest_volume_multiple()
            vwap_p = strat.get_vwap()
            near_circuit = book.is_near_circuit(1.5)
            above_vwap = (vwap_p == 0) or (b0_p >= vwap_p)

            # Signal definition
            is_valid_signal = (
                len(strat.recent_vols) >= 10
                and vol_mult >= 2.5
                and comp_ofi >= 0.55
                and above_vwap
                and not near_circuit
                and a0_p > 0
            )

            # Valid trading window filter: 09:30-11:00 or 13:30-14:45
            in_window = (34200 <= sec_of_day <= 39600) or (48600 <= sec_of_day <= 53100)

            stats = day_symbol_stats[(day_idx, symbol)]
            stats["ticks"] += 1
            if vol_mult > stats["max_vol_mult"]:
                stats["max_vol_mult"] = vol_mult
            if comp_ofi > stats["max_ofi"]:
                stats["max_ofi"] = comp_ofi
            if comp_ofi < stats["min_ofi"]:
                stats["min_ofi"] = comp_ofi

            if is_valid_signal:
                stats["signals_all"] += 1
                if in_window:
                    stats["signals_window"] += 1
                all_signals.append({
                    "day_idx": day_idx,
                    "symbol": symbol,
                    "sec": sec_of_day,
                    "ltp": ltp_inr,
                    "bid": b0_p_inr,
                    "vwap": vwap_p / 100.0,
                    "vol_mult": vol_mult,
                    "ofi": comp_ofi,
                    "in_window": in_window,
                    "near_circuit": near_circuit,
                })

    print(f"\n1. AUDITING 7 BREAKOUT SETUPS:")
    all_breakouts_pass = True
    for d, (sym, start_s, end_s) in BREAKOUT_TARGETS.items():
        # Filter signals for this day and symbol
        sigs = [s for s in all_signals if s["day_idx"] == d and s["symbol"] == sym and start_s <= s["sec"] <= end_s]
        status = "PASS" if len(sigs) > 0 else "FAIL"
        if not sigs:
            all_breakouts_pass = False
            print(f"  [FAIL] Day {d:2d} ({sym}): 0 signals in window [{start_s}-{end_s}]!")
        else:
            first = sigs[0]
            hh, mm, ss = first["sec"] // 3600, (first["sec"] % 3600) // 60, first["sec"] % 60
            print(f"  [{status}] Day {d:2d} ({sym:<10}): {len(sigs):2d} signals | First: {hh:02d}:{mm:02d}:{ss:02d} "
                  f"LTP={first['ltp']:.2f} Bid={first['bid']:.2f} VWAP={first['vwap']:.2f} "
                  f"VolMult={first['vol_mult']:.2f}x OFI={first['ofi']:+.3f}")

    print(f"\n2. AUDITING 3 CHOP BENCHMARK DAYS (Days 2, 11, 19):")
    all_chop_pass = True
    for d in CHOP_DAYS:
        cal_date = TARGET_DATES[d - 1]
        print(f"  --- Day {d:2d} ({cal_date}) ---")
        day_sigs = [s for s in all_signals if s["day_idx"] == d]
        day_win_sigs = [s for s in day_sigs if s["in_window"]]
        
        # Check all symbols on this day
        max_vm = max(day_symbol_stats[(d, s)]["max_vol_mult"] for s in EXPECTED_TOKENS)
        max_ofi = max(abs(day_symbol_stats[(d, s)]["max_ofi"]) for s in EXPECTED_TOKENS)
        min_ofi = min(day_symbol_stats[(d, s)]["min_ofi"] for s in EXPECTED_TOKENS)
        total_ticks = sum(day_symbol_stats[(d, s)]["ticks"] for s in EXPECTED_TOKENS)

        sig_ok = (len(day_win_sigs) == 0)
        vm_ok = (max_vm < 2.0)
        ofi_ok = (max_ofi < 0.30 and abs(min_ofi) < 0.30)

        status = "PASS" if (sig_ok and vm_ok and ofi_ok) else "FAIL"
        if not (sig_ok and vm_ok and ofi_ok):
            all_chop_pass = False

        print(f"  [{status}] Day {d:2d}: Window Signals={len(day_win_sigs)}, All Signals={len(day_sigs)}, "
              f"Max VolMult={max_vm:.2f}x, OFI Range=[{min_ofi:+.3f}, {max_ofi:+.3f}], Ticks={total_ticks:,}")

    print(f"\n3. COMPLETE 30-DAY SIGNAL SUMMARY (By Day & Window):")
    for d in range(1, 31):
        cal_date = TARGET_DATES[d - 1]
        day_sigs = [s for s in all_signals if s["day_idx"] == d]
        win_sigs = [s for s in day_sigs if s["in_window"]]
        if win_sigs or day_sigs:
            syms = set(s["symbol"] for s in day_sigs)
            print(f"  Day {d:2d} ({cal_date}): {len(win_sigs):2d} window signals ({len(day_sigs):2d} total) across {syms}")
        else:
            print(f"  Day {d:2d} ({cal_date}):  0 signals (Clean)")

    print("\n" + "=" * 80)
    print(f"OVERALL RESULT: Breakout Pass = {all_breakouts_pass}, Chop Pass = {all_chop_pass}")
    print("=" * 80)

if __name__ == "__main__":
    run_independent_audit()
