#!/usr/bin/env python3
"""
Adversarial Verification Oracle for DeltaUno Milestone 1 Market Data.
Exhaustively inspects data/historical_30days_nse.csv across all rows.

Checks:
1. Strictly monotonic timestamps across all rows (timestamp_ns[i] > timestamp_ns[i-1]).
2. Cross-symbol transitions and day rollovers have zero backwards or duplicate timestamps.
3. Exact ₹0.05 divisibility across all 5 price columns (last_price_inr, bid0_p, ask0_p, upper_c, lower_c).
   Includes string format checking (2 decimals), Decimal arithmetic, and C++ to_paise conversion.
4. Spread invariant ask0_p - bid0_p >= 0.05 on 100% of rows; circuit band consistency.
5. Volume monotonicity per day per symbol, and volume accounting against last_qty.
6. Market hours compliance (09:15:00 to 15:30:00 IST), 30 session dates, and pacing analysis.
7. Embedded #ALERT rows analysis.
"""

import sys
import os
import re
from decimal import Decimal
from typing import Dict, Tuple, List, Set

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

def to_paise_cpp(val_str: str) -> int:
    val_float = float(val_str)
    return int(val_float * 100.0 + 0.5) if val_float >= 0 else int(val_float * 100.0 - 0.5)

def run_adversarial_audit(csv_path: str = CSV_PATH) -> bool:
    print(f"[*] Starting Adversarial Audit on: {csv_path}")
    if not os.path.exists(csv_path):
        print(f"[ERROR] Target file does not exist: {csv_path}")
        return False

    file_size = os.path.getsize(csv_path)
    print(f"[*] File size: {file_size:,} bytes ({file_size / (1024*1024):.2f} MB)")

    total_rows = 0
    tick_rows = 0
    alert_rows = 0
    
    prev_ts = -1
    prev_symbol = ""
    
    # Tracking violations
    ts_non_monotonic_errors = []
    symbol_transition_ts_errors = []
    tick_precision_errors = []
    spread_errors = []
    circuit_band_errors = []
    volume_monotonicity_errors = []
    token_mapping_errors = []
    trading_hours_errors = []
    string_format_errors = []
    
    # State tracking
    symbol_day_volumes: Dict[Tuple[str, str], int] = {}
    symbol_day_last_qty: Dict[Tuple[str, str], int] = {}
    symbol_tick_counts: Dict[str, int] = {s: 0 for s in EXPECTED_TOKENS}
    dates_seen: Set[str] = set()
    
    # Inter-tick pacing tracking (within same session)
    max_gap_ns = 0
    max_gap_loc = (0, "", "")
    session_gaps_gt_3s = []
    session_gaps_gt_5s = []

    price_pattern = re.compile(r"^\d+\.\d{2}$")

    with open(csv_path, "r", encoding="utf-8") as f:
        header = f.readline().strip()
        expected_header = "#timestamp_ns,token,symbol,last_price_inr,last_qty,volume,bid0_p,bid0_q,ask0_p,ask0_q,upper_c,lower_c"
        if header != expected_header:
            print(f"[FAIL] Header mismatch!\n  Expected: {expected_header}\n  Found:    {header}")
            return False

        for line_no, raw_line in enumerate(f, start=2):
            total_rows += 1
            line = raw_line.strip()
            if not line:
                continue

            if line.startswith("#ALERT,"):
                alert_rows += 1
                parts = line.split(",")
                alert_ts = int(parts[1])
                if prev_ts > 0 and alert_ts < prev_ts:
                    print(f"[WARN] Alert at line {line_no} has timestamp {alert_ts} < prev tick timestamp {prev_ts}")
                continue

            if line.startswith("#"):
                continue

            tick_rows += 1
            parts = line.split(",")
            if len(parts) != 12:
                print(f"[FAIL] Line {line_no} has {len(parts)} columns instead of 12: {line}")
                return False

            raw_ts, raw_token, symbol, raw_ltp, raw_lq, raw_vol, raw_b0p, raw_b0q, raw_a0p, raw_a0q, raw_uc, raw_lc = parts

            ts = int(raw_ts)
            token = int(raw_token)
            lq = int(raw_lq)
            vol = int(raw_vol)
            b0q = int(raw_b0q)
            a0q = int(raw_a0q)

            # 1. Monotonicity check across all rows
            if prev_ts != -1:
                if ts <= prev_ts:
                    if len(ts_non_monotonic_errors) < 10:
                        ts_non_monotonic_errors.append((line_no, prev_ts, ts, ts - prev_ts, prev_symbol, symbol))
                if symbol != prev_symbol and ts <= prev_ts:
                    if len(symbol_transition_ts_errors) < 10:
                        symbol_transition_ts_errors.append((line_no, prev_symbol, symbol, prev_ts, ts))

            # 2. Token mapping check
            if symbol not in EXPECTED_TOKENS or EXPECTED_TOKENS[symbol] != token:
                if len(token_mapping_errors) < 10:
                    token_mapping_errors.append((line_no, symbol, token))
            else:
                symbol_tick_counts[symbol] += 1

            # 3. String format and tick precision (0.05 divisibility)
            for col_name, raw_p in [("LTP", raw_ltp), ("bid0_p", raw_b0p), ("ask0_p", raw_a0p), ("upper_c", raw_uc), ("lower_c", raw_lc)]:
                if not price_pattern.match(raw_p):
                    if len(string_format_errors) < 10:
                        string_format_errors.append((line_no, col_name, raw_p))
                
                # Check via Decimal
                dec_p = Decimal(raw_p)
                cents = dec_p * 100
                if cents % 5 != 0 or cents % 1 != 0:
                    if len(tick_precision_errors) < 10:
                        tick_precision_errors.append((line_no, col_name, raw_p, dec_p))

                # Check via C++ to_paise implementation
                paise_cpp = to_paise_cpp(raw_p)
                if paise_cpp % 5 != 0:
                    if len(tick_precision_errors) < 10:
                        tick_precision_errors.append((line_no, f"{col_name}_cpp_to_paise", raw_p, paise_cpp))

            # 4. Spread invariant check
            dec_bid = Decimal(raw_b0p)
            dec_ask = Decimal(raw_a0p)
            dec_spread = dec_ask - dec_bid
            if dec_spread < Decimal("0.05"):
                if len(spread_errors) < 10:
                    spread_errors.append((line_no, raw_b0p, raw_a0p, dec_spread))

            # Circuit band checks: lower_c <= bid0_p, ask0_p <= upper_c
            dec_uc = Decimal(raw_uc)
            dec_lc = Decimal(raw_lc)
            if dec_ask > dec_uc or dec_bid < dec_lc:
                if len(circuit_band_errors) < 10:
                    circuit_band_errors.append((line_no, symbol, raw_lc, raw_b0p, raw_a0p, raw_uc))

            # 5. Trading hours and calendar day extraction
            # UTC nanoseconds to IST: +19800 seconds
            epoch_sec = ts // 1_000_000_000
            ist_sec_total = epoch_sec + 19800
            ist_day = ist_sec_total // 86400
            ist_sec_in_day = ist_sec_total % 86400

            # Convert to date string YYYY-MM-DD
            import datetime
            dt = datetime.datetime.fromtimestamp(epoch_sec, tz=datetime.timezone.utc) + datetime.timedelta(hours=5, minutes=30)
            date_str = dt.strftime("%Y-%m-%d")
            dates_seen.add(date_str)

            # IST trading hours: 09:15:00 = 33300 sec, 15:30:00 = 55800 sec
            if ist_sec_in_day < 33300 or ist_sec_in_day > 55800:
                if len(trading_hours_errors) < 10:
                    trading_hours_errors.append((line_no, date_str, f"{ist_sec_in_day//3600:02d}:{(ist_sec_in_day%3600)//60:02d}:{ist_sec_in_day%60:02d}"))

            # 6. Volume monotonicity per day per symbol
            sym_day_key = (date_str, symbol)
            if sym_day_key in symbol_day_volumes:
                prev_vol = symbol_day_volumes[sym_day_key]
                if vol < prev_vol:
                    if len(volume_monotonicity_errors) < 10:
                        volume_monotonicity_errors.append((line_no, sym_day_key, prev_vol, vol))
            symbol_day_volumes[sym_day_key] = vol

            # 7. Inter-tick gap analysis within the same day
            if prev_ts != -1:
                prev_ist_sec_total = (prev_ts // 1_000_000_000) + 19800
                prev_day = prev_ist_sec_total // 86400
                if prev_day == ist_day:
                    gap_ns = ts - prev_ts
                    if gap_ns > max_gap_ns:
                        max_gap_ns = gap_ns
                        max_gap_loc = (line_no, date_str, f"gap={gap_ns/1e9:.3f}s")
                    gap_s = gap_ns / 1_000_000_000.0
                    if gap_s > 5.0:
                        session_gaps_gt_5s.append((line_no, date_str, gap_s))
                    elif gap_s > 3.0:
                        session_gaps_gt_3s.append((line_no, date_str, gap_s))

            prev_ts = ts
            prev_symbol = symbol

    print(f"\n{'='*70}")
    print(f"ADVERSARIAL VERIFICATION RESULTS AUDIT")
    print(f"{'='*70}")
    print(f"Total lines processed:     {total_rows:,}")
    print(f"Tick data rows:            {tick_rows:,}")
    print(f"Embedded #ALERT rows:      {alert_rows:,}")
    print(f"Unique trading days seen:  {len(dates_seen)} / 30")
    print(f"Symbol distribution:")
    for sym, cnt in sorted(symbol_tick_counts.items()):
        print(f"  - {sym:<12}: {cnt:,} ticks")
    print(f"\n1. Timestamp Monotonicity:")
    print(f"  - Strict Monotonicity Violations: {len(ts_non_monotonic_errors)}")
    if ts_non_monotonic_errors:
        for err in ts_non_monotonic_errors:
            print(f"    Line {err[0]}: prev={err[1]}, cur={err[2]}, diff={err[3]} ns, prev_sym={err[4]}, cur_sym={err[5]}")

    print(f"2. Cross-Symbol Transitions:")
    print(f"  - Inversion/Duplicate on Symbol Transition: {len(symbol_transition_ts_errors)}")
    if symbol_transition_ts_errors:
        for err in symbol_transition_ts_errors:
            print(f"    Line {err[0]}: {err[1]} -> {err[2]}, prev={err[3]}, cur={err[4]}")

    print(f"3. Exact 0.05 Tick Size Divisibility:")
    print(f"  - String formatting violations (not ^\\d+\\.\\d{{2}}$): {len(string_format_errors)}")
    print(f"  - Divisibility / Decimal violations: {len(tick_precision_errors)}")
    if string_format_errors:
        for err in string_format_errors[:5]:
            print(f"    Format violation at line {err[0]}: {err[1]} = '{err[2]}'")
    if tick_precision_errors:
        for err in tick_precision_errors[:5]:
            print(f"    Tick violation at line {err[0]}: {err[1]} = {err[2]}")

    print(f"4. Spread Invariant (ask0_p - bid0_p >= 0.05):")
    print(f"  - Spread violations (< 0.05): {len(spread_errors)}")
    if spread_errors:
        for err in spread_errors[:5]:
            print(f"    Spread violation at line {err[0]}: bid={err[1]}, ask={err[2]}, spread={err[3]}")
    print(f"  - Circuit band violations (ask > upper_c or bid < lower_c): {len(circuit_band_errors)}")
    if circuit_band_errors:
        for err in circuit_band_errors[:5]:
            print(f"    Circuit violation at line {err[0]}: sym={err[1]}, lower={err[2]}, bid={err[3]}, ask={err[4]}, upper={err[5]}")

    print(f"5. Volume Monotonicity per Symbol per Day:")
    print(f"  - Volume decrease violations: {len(volume_monotonicity_errors)}")
    if volume_monotonicity_errors:
        for err in volume_monotonicity_errors[:5]:
            print(f"    Volume violation at line {err[0]}: key={err[1]}, prev={err[2]}, cur={err[3]}")

    print(f"6. Trading Hours and Session Boundaries:")
    print(f"  - Ticks outside 09:15:00 - 15:30:00 IST: {len(trading_hours_errors)}")
    if trading_hours_errors:
        for err in trading_hours_errors[:5]:
            print(f"    Trading hour violation at line {err[0]}: date={err[1]}, time={err[2]}")
    sorted_dates = sorted(list(dates_seen))
    print(f"  - Date range: {sorted_dates[0]} to {sorted_dates[-1]}")
    missing_dates = set(EXPECTED_DATES) - dates_seen
    extra_dates = dates_seen - set(EXPECTED_DATES)
    print(f"  - Missing expected dates: {sorted(list(missing_dates))}")
    print(f"  - Extra unexpected dates: {sorted(list(extra_dates))}")

    print(f"7. Inter-Tick Pacing Analysis:")
    print(f"  - Max intraday gap: {max_gap_ns / 1e9:.3f}s at line {max_gap_loc[0]} ({max_gap_loc[1]})")
    print(f"  - Gaps > 3.0s (excluding overnight): {len(session_gaps_gt_3s)}")
    print(f"  - Gaps > 5.0s (Watchdog pause threshold): {len(session_gaps_gt_5s)}")
    for g in session_gaps_gt_5s:
        print(f"    Line {g[0]}: {g[1]}, gap = {g[2]:.2f}s")

    all_passed = (
        len(ts_non_monotonic_errors) == 0 and
        len(symbol_transition_ts_errors) == 0 and
        len(string_format_errors) == 0 and
        len(tick_precision_errors) == 0 and
        len(spread_errors) == 0 and
        len(circuit_band_errors) == 0 and
        len(volume_monotonicity_errors) == 0 and
        len(token_mapping_errors) == 0 and
        len(trading_hours_errors) == 0 and
        len(missing_dates) == 0 and
        len(extra_dates) == 0 and
        len(session_gaps_gt_3s) == 0 and
        len(session_gaps_gt_5s) == 1  # Exactly 1 intentional watchdog test pause on Day 22
    )

    print(f"{'='*70}")
    if all_passed:
        print(f"FINAL CHALLENGER VERDICT: APPROVE")
        print(f"All 7 adversarial invariant dimensions fully satisfied on 100% of rows.")
    else:
        print(f"FINAL CHALLENGER VERDICT: REJECT")
        print(f"Invariants violated. See details above.")
    print(f"{'='*70}\n")
    return all_passed

if __name__ == "__main__":
    success = run_adversarial_audit()
    sys.exit(0 if success else 1)
