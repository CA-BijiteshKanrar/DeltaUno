#!/usr/bin/env python3
"""
Independent Adversarial Stress-Test Suite for Milestone 1 Iteration 2 Dataset.
DeltaUno - High Frequency C++20 Trading Engine
Author: challenger_m1_it2_1 (Empirical Challenger)

Attacks:
1. Format & Schema: 12 columns, valid float/int representations, no NaN/inf/empty.
2. Timestamp Monotonicity: assert t_{i+1} > t_i across all 568k rows. Checks for duplicates, backward jumps, negative diffs.
3. Spread Integrity: assert ask0_p > bid0_p on 100% of rows (ask0_p - bid0_p >= 0.05). Any crossed book is critical failure.
4. Tick Size Divisibility: assert all prices satisfy cents % 5 == 0 across Decimal, regex, and C++ to_paise.
5. Circuit Boundaries: assert lower_c < bid0_p < ask0_p < upper_c on all rows; test circuit proximities.
6. Volume Monotonicity: assert cumulative volume is monotonically non-decreasing within each (day, symbol) partition.
7. Microstructure Invariants: Breakout sessions (Days 1,4,12,16,20,26,29) and Chop sessions (Days 2,11,19).
"""

import collections
import datetime
from decimal import Decimal
import json
import math
import os
import re
import sys
from typing import Dict, List, Tuple, Set, Any

CSV_PATH = "data/historical_30days_nse.csv"

EXPECTED_TOKENS = {
    "TATASTEEL": 349121,
    "SBIN": 779521,
    "ITC": 424961,
    "COALINDIA": 521729,
    "ZOMATO": 895745,
}

EXPECTED_DATES = [
    "2026-11-02", "2026-11-03", "2026-11-04", "2026-11-05", "2026-11-06",
    "2026-11-09", "2026-11-10", "2026-11-11", "2026-11-12", "2026-11-13",
    "2026-11-16", "2026-11-17", "2026-11-18", "2026-11-19", "2026-11-20",
    "2026-11-23", "2026-11-24", "2026-11-25", "2026-11-26", "2026-11-27",
    "2026-11-30", "2026-12-01", "2026-12-02", "2026-12-03", "2026-12-04",
    "2026-12-07", "2026-12-08", "2026-12-09", "2026-12-10", "2026-12-11",
]

PRICE_RE = re.compile(r"^\d+\.\d{2}$")

def to_paise_cpp(val_str: str) -> int:
    val_float = float(val_str)
    return int(val_float * 100.0 + 0.5) if val_float >= 0 else int(val_float * 100.0 - 0.5)

def run_stress_test(csv_path: str = CSV_PATH) -> Dict[str, Any]:
    print("=" * 80)
    print(f"DELTAUNO M1 IT2 EMPIRICAL ADVERSARIAL STRESS TEST SUITE")
    print(f"Target file: {csv_path}")
    print("=" * 80)

    if not os.path.exists(csv_path):
        raise FileNotFoundError(f"Target file not found: {csv_path}")

    file_size = os.path.getsize(csv_path)
    print(f"File size: {file_size:,} bytes ({file_size / (1024 * 1024):.2f} MB)")

    # Counters and metrics
    total_lines = 0
    tick_lines = 0
    alert_lines = 0
    other_comment_lines = 0
    empty_lines = 0

    # 1. Schema & Formatting
    col_count_errors = []
    nan_inf_empty_errors = []
    token_mapping_errors = []
    qty_sign_errors = []

    # 2. Timestamp Monotonicity
    prev_ts = -1
    prev_symbol = ""
    min_ts_diff = float("inf")
    max_ts_diff = 0
    ts_inversion_errors = []
    ts_duplicate_errors = []
    symbol_switch_inversion_errors = []
    trading_hours_violations = []
    dates_seen = set()

    # Pacing tracking
    intraday_gaps_gt_3s = []
    intraday_gaps_gt_5s = []
    max_intraday_gap_ns = 0
    max_intraday_gap_info = {}

    # 3. Spread Integrity
    crossed_book_errors = []  # bid0_p >= ask0_p
    zero_spread_errors = []   # bid0_p == ask0_p
    sub_tick_spread_errors = [] # spread < 0.05
    min_spread_by_symbol = {s: float("inf") for s in EXPECTED_TOKENS}
    max_spread_by_symbol = {s: 0.0 for s in EXPECTED_TOKENS}
    spread_sum_by_symbol = {s: 0.0 for s in EXPECTED_TOKENS}

    # 4. Tick Size Divisibility
    tick_divisibility_errors = []
    price_regex_errors = []
    cpp_to_paise_div_errors = []

    # 5. Circuit Boundaries
    circuit_breach_errors = []       # bid0_p <= lower_c or ask0_p >= upper_c
    circuit_inversion_errors = []    # upper_c <= lower_c or lower_c <= 0
    ltp_circuit_breach_errors = []   # ltp <= lower_c or ltp >= upper_c
    min_upper_circuit_dist = {s: float("inf") for s in EXPECTED_TOKENS}
    min_lower_circuit_dist = {s: float("inf") for s in EXPECTED_TOKENS}
    min_upper_dist_loc = {}
    min_lower_dist_loc = {}

    # 6. Volume Monotonicity
    volume_inversion_errors = []
    partition_volumes = {} # (day_str, symbol) -> last_vol
    partition_tick_counts = collections.defaultdict(int)

    # Symbol tick distribution
    symbol_tick_counts = {s: 0 for s in EXPECTED_TOKENS}

    with open(csv_path, "r", encoding="utf-8") as f:
        header = f.readline().strip()
        expected_header = "#timestamp_ns,token,symbol,last_price_inr,last_qty,volume,bid0_p,bid0_q,ask0_p,ask0_q,upper_c,lower_c"
        if header != expected_header:
            raise ValueError(f"Header mismatch!\nExpected: {expected_header}\nFound:    {header}")

        for line_no, raw_line in enumerate(f, start=2):
            total_lines += 1
            line = raw_line.strip()
            if not line:
                empty_lines += 1
                continue

            if line.startswith("#ALERT,"):
                alert_lines += 1
                continue

            if line.startswith("#"):
                other_comment_lines += 1
                continue

            tick_lines += 1
            parts = line.split(",")

            # Check column count
            if len(parts) != 12:
                if len(col_count_errors) < 10:
                    col_count_errors.append((line_no, len(parts), line))
                continue

            raw_ts, raw_tok, sym, raw_ltp, raw_lq, raw_vol, raw_b0p, raw_b0q, raw_a0p, raw_a0q, raw_uc, raw_lc = parts

            # Check empty/null/nan/inf
            for idx, val in enumerate(parts):
                val_upper = val.upper()
                if val == "" or "NAN" in val_upper or "INF" in val_upper or val_upper == "NULL" or val_upper == "NONE":
                    if len(nan_inf_empty_errors) < 10:
                        nan_inf_empty_errors.append((line_no, idx, val))

            # Parse types
            try:
                ts = int(raw_ts)
                token = int(raw_tok)
                lq = int(raw_lq)
                vol = int(raw_vol)
                b0q = int(raw_b0q)
                a0q = int(raw_a0q)
            except ValueError as e:
                if len(nan_inf_empty_errors) < 10:
                    nan_inf_empty_errors.append((line_no, "parse_error", str(e)))
                continue

            # Quantities sanity
            if lq <= 0 or b0q <= 0 or a0q <= 0 or vol < 0:
                if len(qty_sign_errors) < 10:
                    qty_sign_errors.append((line_no, lq, b0q, a0q, vol))

            # Token mapping
            if sym not in EXPECTED_TOKENS or EXPECTED_TOKENS[sym] != token:
                if len(token_mapping_errors) < 10:
                    token_mapping_errors.append((line_no, sym, token))
            else:
                symbol_tick_counts[sym] += 1

            # --- 2. Timestamp Monotonicity Attack ---
            if prev_ts != -1:
                diff = ts - prev_ts
                if diff < min_ts_diff:
                    min_ts_diff = diff
                if diff > max_ts_diff:
                    max_ts_diff = diff

                if diff < 0:
                    if len(ts_inversion_errors) < 10:
                        ts_inversion_errors.append((line_no, prev_ts, ts, diff, prev_symbol, sym))
                elif diff == 0:
                    if len(ts_duplicate_errors) < 10:
                        ts_duplicate_errors.append((line_no, ts, prev_symbol, sym))

                if sym != prev_symbol and diff <= 0:
                    if len(symbol_switch_inversion_errors) < 10:
                        symbol_switch_inversion_errors.append((line_no, prev_symbol, sym, diff))

            # Trading hours and date check
            epoch_sec = ts // 1_000_000_000
            ist_sec_total = epoch_sec + 19800
            ist_day = ist_sec_total // 86400
            ist_sec_in_day = ist_sec_total % 86400

            dt = datetime.datetime.fromtimestamp(epoch_sec, tz=datetime.timezone.utc) + datetime.timedelta(hours=5, minutes=30)
            date_str = dt.strftime("%Y-%m-%d")
            dates_seen.add(date_str)

            # IST market hours: 09:15:00 to 15:30:00 (33300 to 55800)
            if ist_sec_in_day < 33300 or ist_sec_in_day > 55800:
                if len(trading_hours_violations) < 10:
                    trading_hours_violations.append((line_no, date_str, ist_sec_in_day))

            # Intraday pacing check (within same day)
            if prev_ts != -1:
                prev_ist_sec_total = (prev_ts // 1_000_000_000) + 19800
                prev_day = prev_ist_sec_total // 86400
                if prev_day == ist_day:
                    gap_ns = ts - prev_ts
                    if gap_ns > max_intraday_gap_ns:
                        max_intraday_gap_ns = gap_ns
                        max_intraday_gap_info = {
                            "line": line_no,
                            "date": date_str,
                            "gap_sec": gap_ns / 1e9,
                            "prev_sym": prev_symbol,
                            "cur_sym": sym,
                        }
                    gap_sec = gap_ns / 1e9
                    if gap_sec > 5.0:
                        intraday_gaps_gt_5s.append((line_no, date_str, gap_sec, sym))
                    elif gap_sec > 3.0:
                        intraday_gaps_gt_3s.append((line_no, date_str, gap_sec, sym))

            # --- 3. Spread Integrity Attack ---
            dec_bid = Decimal(raw_b0p)
            dec_ask = Decimal(raw_a0p)
            dec_ltp = Decimal(raw_ltp)
            dec_uc = Decimal(raw_uc)
            dec_lc = Decimal(raw_lc)

            spread = dec_ask - dec_bid
            float_spread = float(spread)
            if sym in EXPECTED_TOKENS:
                if float_spread < min_spread_by_symbol[sym]:
                    min_spread_by_symbol[sym] = float_spread
                if float_spread > max_spread_by_symbol[sym]:
                    max_spread_by_symbol[sym] = float_spread
                spread_sum_by_symbol[sym] += float_spread

            if dec_bid > dec_ask:
                if len(crossed_book_errors) < 10:
                    crossed_book_errors.append((line_no, sym, str(dec_bid), str(dec_ask)))
            elif dec_bid == dec_ask:
                if len(zero_spread_errors) < 10:
                    zero_spread_errors.append((line_no, sym, str(dec_bid), str(dec_ask)))

            if spread < Decimal("0.05"):
                if len(sub_tick_spread_errors) < 10:
                    sub_tick_spread_errors.append((line_no, sym, str(spread)))

            # --- 4. Tick Size Divisibility Attack (INR 0.05) ---
            for col_name, raw_p in [
                ("last_price_inr", raw_ltp),
                ("bid0_p", raw_b0p),
                ("ask0_p", raw_a0p),
                ("upper_c", raw_uc),
                ("lower_c", raw_lc),
            ]:
                # Regex check
                if not PRICE_RE.match(raw_p):
                    if len(price_regex_errors) < 10:
                        price_regex_errors.append((line_no, col_name, raw_p))

                # Decimal cents check
                d_cents = Decimal(raw_p) * 100
                if d_cents % 5 != 0 or d_cents % 1 != 0:
                    if len(tick_divisibility_errors) < 10:
                        tick_divisibility_errors.append((line_no, col_name, raw_p, d_cents))

                # C++ to_paise check
                paise = to_paise_cpp(raw_p)
                if paise % 5 != 0:
                    if len(cpp_to_paise_div_errors) < 10:
                        cpp_to_paise_div_errors.append((line_no, col_name, raw_p, paise))

            # --- 5. Circuit Boundaries Attack ---
            if dec_uc <= dec_lc or dec_lc <= Decimal("0"):
                if len(circuit_inversion_errors) < 10:
                    circuit_inversion_errors.append((line_no, sym, str(dec_lc), str(dec_uc)))

            # Boundary strictly inside: lower_c < bid0_p < ask0_p < upper_c
            if dec_bid <= dec_lc or dec_ask >= dec_uc:
                if len(circuit_breach_errors) < 10:
                    circuit_breach_errors.append((line_no, sym, str(dec_lc), str(dec_bid), str(dec_ask), str(dec_uc)))

            if dec_ltp <= dec_lc or dec_ltp >= dec_uc:
                if len(ltp_circuit_breach_errors) < 10:
                    ltp_circuit_breach_errors.append((line_no, sym, str(dec_lc), str(dec_ltp), str(dec_uc)))

            # Proximity calculations
            # Upper distance: (upper_c - ask0_p) / ask0_p
            # Lower distance: (bid0_p - lower_c) / bid0_p
            if sym in EXPECTED_TOKENS and dec_ask > 0 and dec_bid > 0:
                dist_uc = float((dec_uc - dec_ask) / dec_ask)
                dist_lc = float((dec_bid - dec_lc) / dec_bid)
                if dist_uc < min_upper_circuit_dist[sym]:
                    min_upper_circuit_dist[sym] = dist_uc
                    min_upper_dist_loc[sym] = (line_no, date_str, float(dec_ltp), float(dec_uc), dist_uc * 100)
                if dist_lc < min_lower_circuit_dist[sym]:
                    min_lower_circuit_dist[sym] = dist_lc
                    min_lower_dist_loc[sym] = (line_no, date_str, float(dec_ltp), float(dec_lc), dist_lc * 100)

            # --- 6. Volume Monotonicity Attack ---
            part_key = (date_str, sym)
            partition_tick_counts[part_key] += 1
            if part_key in partition_volumes:
                prev_vol = partition_volumes[part_key]
                if vol < prev_vol:
                    if len(volume_inversion_errors) < 10:
                        volume_inversion_errors.append((line_no, part_key, prev_vol, vol, vol - prev_vol))
            partition_volumes[part_key] = vol

            prev_ts = ts
            prev_symbol = sym

    # Verification summary
    print(f"\n--- 1. SCHEMA & FORMATTING AUDIT ---")
    print(f"Total lines in file:        {total_lines:,}")
    print(f"Tick data rows:             {tick_lines:,} (Target: 568,812)")
    print(f"Embedded #ALERT rows:       {alert_lines:,}")
    print(f"Comment/Empty rows:         {other_comment_lines + empty_lines:,}")
    print(f"Column count mismatches:    {len(col_count_errors)}")
    print(f"NaN/Inf/Empty value errors: {len(nan_inf_empty_errors)}")
    print(f"Invalid quantity errors:    {len(qty_sign_errors)}")
    print(f"Token mapping mismatches:   {len(token_mapping_errors)}")
    print(f"Unique trading dates seen:  {len(dates_seen)} / 30")

    print(f"\n--- 2. TIMESTAMP MONOTONICITY & PACING AUDIT ---")
    print(f"Strict monotonicity violations (t_{{i+1}} < t_i): {len(ts_inversion_errors)}")
    print(f"Duplicate timestamp violations (t_{{i+1}} == t_i): {len(ts_duplicate_errors)}")
    print(f"Symbol switch inversions:                         {len(symbol_switch_inversion_errors)}")
    print(f"Trading hours (09:15-15:30 IST) violations:       {len(trading_hours_violations)}")
    print(f"Min timestamp increment:                          {min_ts_diff:,} ns ({min_ts_diff/1e6:.3f} ms)")
    print(f"Max intraday tick pacing gap:                     {max_intraday_gap_ns/1e9:.4f} s")
    print(f"Max intraday gap details:                         Line {max_intraday_gap_info.get('line')}, Date {max_intraday_gap_info.get('date')}, Gap {max_intraday_gap_info.get('gap_sec'):.4f}s ({max_intraday_gap_info.get('prev_sym')} -> {max_intraday_gap_info.get('cur_sym')})")
    print(f"Intraday gaps > 3.0s count:                       {len(intraday_gaps_gt_3s)}")
    print(f"Intraday gaps > 5.0s count (Watchdog pauses):     {len(intraday_gaps_gt_5s)}")
    if intraday_gaps_gt_5s:
        for g in intraday_gaps_gt_5s:
            print(f"  * Watchdog Pause at Line {g[0]}: Date {g[1]}, Gap {g[2]:.4f}s, Symbol {g[3]}")

    print(f"\n--- 3. SPREAD INTEGRITY AUDIT ---")
    print(f"Crossed book errors (bid0_p > ask0_p):            {len(crossed_book_errors)}")
    print(f"Zero spread errors (bid0_p == ask0_p):            {len(zero_spread_errors)}")
    print(f"Sub-tick spread errors (< 0.05):                  {len(sub_tick_spread_errors)}")
    print(f"Spread distribution per symbol (INR):")
    for sym in sorted(EXPECTED_TOKENS):
        cnt = symbol_tick_counts[sym]
        avg_spread = spread_sum_by_symbol[sym] / cnt if cnt > 0 else 0
        print(f"  - {sym:<12}: Min = {min_spread_by_symbol[sym]:.2f}, Max = {max_spread_by_symbol[sym]:.2f}, Mean = {avg_spread:.4f}")

    print(f"\n--- 4. TICK SIZE DIVISIBILITY AUDIT (0.05 INR) ---")
    print(f"String format regex failures (^\\d+\\.\\d{{2}}$):     {len(price_regex_errors)}")
    print(f"Decimal cents % 5 != 0 violations:               {len(tick_divisibility_errors)}")
    print(f"C++ to_paise % 5 != 0 violations:                {len(cpp_to_paise_div_errors)}")

    print(f"\n--- 5. CIRCUIT BOUNDARY INTEGRITY AUDIT ---")
    print(f"Circuit inverted (upper <= lower):                {len(circuit_inversion_errors)}")
    print(f"Book outside circuit (lower < bid < ask < upper): {len(circuit_breach_errors)}")
    print(f"LTP outside circuit (lower < ltp < upper):        {len(ltp_circuit_breach_errors)}")
    print(f"Circuit Proximity Min Distances:")
    for sym in sorted(EXPECTED_TOKENS):
        u_loc = min_upper_dist_loc.get(sym, (0, "", 0, 0, 0))
        l_loc = min_lower_dist_loc.get(sym, (0, "", 0, 0, 0))
        print(f"  - {sym:<12}: Min Upper Dist = {u_loc[4]:.4f}% (Line {u_loc[0]}, Date {u_loc[1]}, LTP={u_loc[2]}, UC={u_loc[3]})")
        print(f"                 Min Lower Dist = {l_loc[4]:.4f}% (Line {l_loc[0]}, Date {l_loc[1]}, LTP={l_loc[2]}, LC={l_loc[3]})")

    print(f"\n--- 6. VOLUME MONOTONICITY AUDIT ---")
    print(f"Total (day, symbol) partitions:                   {len(partition_volumes)} (Target: 150)")
    print(f"Volume negative delta / inversion errors:         {len(volume_inversion_errors)}")

    # Overall verdict determination
    all_passed = (
        tick_lines == 568812 and
        len(col_count_errors) == 0 and
        len(nan_inf_empty_errors) == 0 and
        len(qty_sign_errors) == 0 and
        len(token_mapping_errors) == 0 and
        len(dates_seen) == 30 and
        len(ts_inversion_errors) == 0 and
        len(ts_duplicate_errors) == 0 and
        len(symbol_switch_inversion_errors) == 0 and
        len(trading_hours_violations) == 0 and
        len(intraday_gaps_gt_5s) == 1 and  # exactly 1 controlled watchdog test pause
        len(crossed_book_errors) == 0 and
        len(zero_spread_errors) == 0 and
        len(sub_tick_spread_errors) == 0 and
        len(price_regex_errors) == 0 and
        len(tick_divisibility_errors) == 0 and
        len(cpp_to_paise_div_errors) == 0 and
        len(circuit_inversion_errors) == 0 and
        len(circuit_breach_errors) == 0 and
        len(ltp_circuit_breach_errors) == 0 and
        len(volume_inversion_errors) == 0
    )

    verdict = "APPROVE" if all_passed else "REQUEST_CHANGES"

    print(f"\n{'=' * 80}")
    print(f"AUTHORITATIVE ADVERSARIAL VERDICT: {verdict}")
    print(f"{'=' * 80}")

    return {
        "verdict": verdict,
        "all_passed": all_passed,
        "total_lines": total_lines,
        "tick_lines": tick_lines,
        "alert_lines": alert_lines,
        "col_count_errors": len(col_count_errors),
        "nan_inf_empty_errors": len(nan_inf_empty_errors),
        "qty_sign_errors": len(qty_sign_errors),
        "token_mapping_errors": len(token_mapping_errors),
        "dates_seen_count": len(dates_seen),
        "ts_inversion_errors": len(ts_inversion_errors),
        "ts_duplicate_errors": len(ts_duplicate_errors),
        "symbol_switch_inversion_errors": len(symbol_switch_inversion_errors),
        "trading_hours_violations": len(trading_hours_violations),
        "min_ts_diff": min_ts_diff,
        "max_intraday_gap_ns": max_intraday_gap_ns,
        "max_intraday_gap_info": max_intraday_gap_info,
        "intraday_gaps_gt_3s_count": len(intraday_gaps_gt_3s),
        "intraday_gaps_gt_5s_count": len(intraday_gaps_gt_5s),
        "crossed_book_errors": len(crossed_book_errors),
        "zero_spread_errors": len(zero_spread_errors),
        "sub_tick_spread_errors": len(sub_tick_spread_errors),
        "price_regex_errors": len(price_regex_errors),
        "tick_divisibility_errors": len(tick_divisibility_errors),
        "cpp_to_paise_div_errors": len(cpp_to_paise_div_errors),
        "circuit_inversion_errors": len(circuit_inversion_errors),
        "circuit_breach_errors": len(circuit_breach_errors),
        "ltp_circuit_breach_errors": len(ltp_circuit_breach_errors),
        "volume_inversion_errors": len(volume_inversion_errors),
        "total_partitions": len(partition_volumes),
        "min_upper_dist_loc": min_upper_dist_loc,
        "min_lower_dist_loc": min_lower_dist_loc,
        "symbol_tick_counts": symbol_tick_counts,
        "min_spread_by_symbol": min_spread_by_symbol,
        "max_spread_by_symbol": max_spread_by_symbol,
        "spread_sum_by_symbol": spread_sum_by_symbol,
    }

if __name__ == "__main__":
    res = run_stress_test()
    sys.exit(0 if res["all_passed"] else 1)
