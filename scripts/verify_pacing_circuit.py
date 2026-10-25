#!/usr/bin/env python3
"""
Adversarial Verification Script for Milestone 1:
Pacing, Gaps, Watchdog & Circuit Limits Verification.
Author: challenger_m1_2 (Empirical Challenger)
"""

import os
import sys
import datetime
from typing import Dict, List, Tuple

CSV_PATH = "data/historical_30days_nse.csv"

def ns_to_ist_components(ts_ns: int) -> Tuple[str, int, int, int, int]:
    """Returns (date_str, hour, minute, second, sec_from_midnight) in IST."""
    # IST is UTC + 5h30m = +19800s
    total_sec = ts_ns // 1_000_000_000
    ist_sec = total_sec + 19800
    days = ist_sec // 86400
    sec_of_day = ist_sec % 86400
    hour = sec_of_day // 3600
    minute = (sec_of_day % 3600) // 60
    second = sec_of_day % 60
    
    # Calculate YYYY-MM-DD from days since 1970-01-01
    dt = datetime.datetime.fromtimestamp(total_sec, tz=datetime.timezone.utc) + datetime.timedelta(hours=5, minutes=30)
    return dt.strftime("%Y-%m-%d"), hour, minute, second, sec_of_day

def run_adversarial_pacing_and_circuit_check():
    print("=" * 70)
    print(" EMPIRICAL CHALLENGER: PACING, GAPS & CIRCUIT LIMITS AUDIT")
    print(f" Target dataset: {CSV_PATH}")
    print("=" * 70)

    if not os.path.exists(CSV_PATH):
        print(f"CRITICAL ERROR: {CSV_PATH} not found!")
        sys.exit(1)

    # 1. Pacing & Gap tracking
    total_rows = 0
    alert_rows = 0
    prev_ts_ns = 0
    prev_day_date = ""
    
    max_gap_ns = 0
    max_gap_info = {}
    gaps_above_3s = []
    
    # Per-day gap tracking
    daily_stats = {} # date -> { 'count': 0, 'max_gap_s': 0.0, 'gaps_gt_3s': [] }

    # 2. Circuit tracking
    circuit_violations = [] # Price > upper_c or Price < lower_c
    zero_or_inverted_circuits = [] # upper_c <= lower_c or upper_c <= 0 or lower_c <= 0
    
    # Proximity tracking
    # COALINDIA Day 8: upper_c is 506.00
    # ZOMATO Day 27: lower_c is 225.00
    day_8_coalindia_ticks = []
    day_27_zomato_ticks = []

    # General instrument circuit proximity
    min_dist_to_upper_pct = 999.0
    min_dist_to_upper_info = {}
    min_dist_to_lower_pct = 999.0
    min_dist_to_lower_info = {}

    unique_dates = []

    with open(CSV_PATH, "r", encoding="utf-8") as f:
        header = f.readline().strip()
        expected_cols = "#timestamp_ns,token,symbol,last_price_inr,last_qty,volume,bid0_p,bid0_q,ask0_p,ask0_q,upper_c,lower_c"
        if header != expected_cols:
            print(f"[REJECT] Header mismatch: {header}")
            sys.exit(1)

        for line_no, raw_line in enumerate(f, start=2):
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("#ALERT,"):
                alert_rows += 1
                continue

            parts = line.split(",")
            if len(parts) != 12:
                print(f"[REJECT] Line {line_no} malformed with {len(parts)} columns")
                sys.exit(1)

            ts_ns = int(parts[0])
            token = int(parts[1])
            symbol = parts[2]
            ltp = float(parts[3])
            last_qty = int(parts[4])
            vol = int(parts[5])
            bid0_p = float(parts[6])
            bid0_q = int(parts[7])
            ask0_p = float(parts[8])
            ask0_q = int(parts[9])
            upper_c = float(parts[10])
            lower_c = float(parts[11])

            total_rows += 1

            date_str, hour, minute, second, sec_of_day = ns_to_ist_components(ts_ns)
            if not unique_dates or unique_dates[-1] != date_str:
                unique_dates.append(date_str)
                daily_stats[date_str] = {'day_idx': len(unique_dates), 'count': 0, 'max_gap_s': 0.0, 'gaps_gt_3s': []}

            daily_stats[date_str]['count'] += 1
            day_idx = daily_stats[date_str]['day_idx']

            # Active market hours: 09:15:00 to 15:30:00 IST (33,300 to 55,800 sec of day)
            is_active_hours = (33300 <= sec_of_day <= 55800)

            # Monotonicity check
            if prev_ts_ns > 0 and ts_ns <= prev_ts_ns:
                print(f"[REJECT] Timestamp inversion at line {line_no}: ts {ts_ns} <= prev_ts {prev_ts_ns}")
                sys.exit(1)

            # Check gap if both ticks in same session and active trading hours
            if prev_ts_ns > 0 and date_str == prev_day_date and is_active_hours:
                gap_ns = ts_ns - prev_ts_ns
                gap_s = gap_ns / 1_000_000_000.0
                if gap_s > daily_stats[date_str]['max_gap_s']:
                    daily_stats[date_str]['max_gap_s'] = gap_s

                if gap_s > 3.0:
                    info = {
                        'line': line_no,
                        'day_idx': day_idx,
                        'date': date_str,
                        'time': f"{hour:02d}:{minute:02d}:{second:02d}",
                        'gap_s': gap_s,
                        'symbol': symbol,
                        'ts_ns': ts_ns,
                        'prev_ts_ns': prev_ts_ns
                    }
                    gaps_above_3s.append(info)
                    daily_stats[date_str]['gaps_gt_3s'].append(info)
                else:
                    if gap_ns > max_gap_ns:
                        max_gap_ns = gap_ns
                        max_gap_info = {
                            'line': line_no,
                            'day_idx': day_idx,
                            'date': date_str,
                            'time': f"{hour:02d}:{minute:02d}:{second:02d}",
                            'gap_s': gap_s,
                            'symbol': symbol
                        }

            prev_ts_ns = ts_ns
            prev_day_date = date_str

            # Circuit bounds checks
            if upper_c <= lower_c or upper_c <= 0 or lower_c <= 0:
                zero_or_inverted_circuits.append((line_no, symbol, upper_c, lower_c))

            # Test bounds on ltp, bid0_p, ask0_p
            for p, name in [(ltp, "ltp"), (bid0_p, "bid0_p"), (ask0_p, "ask0_p")]:
                if p > upper_c or p < lower_c:
                    circuit_violations.append((line_no, day_idx, date_str, symbol, name, p, lower_c, upper_c))

            # Proximity calculations
            dist_to_upper_pct = (upper_c - ltp) / ltp * 100.0
            dist_to_lower_pct = (ltp - lower_c) / ltp * 100.0

            if dist_to_upper_pct < min_dist_to_upper_pct:
                min_dist_to_upper_pct = dist_to_upper_pct
                min_dist_to_upper_info = {
                    'line': line_no, 'day_idx': day_idx, 'date': date_str,
                    'symbol': symbol, 'ltp': ltp, 'upper_c': upper_c,
                    'dist_pct': dist_to_upper_pct
                }

            if dist_to_lower_pct < min_dist_to_lower_pct:
                min_dist_to_lower_pct = dist_to_lower_pct
                min_dist_to_lower_info = {
                    'line': line_no, 'day_idx': day_idx, 'date': date_str,
                    'symbol': symbol, 'ltp': ltp, 'lower_c': lower_c,
                    'dist_pct': dist_to_lower_pct
                }

            # Collect Day 8 COALINDIA ticks
            if day_idx == 8 and symbol == "COALINDIA":
                day_8_coalindia_ticks.append({
                    'line': line_no, 'time': f"{hour:02d}:{minute:02d}:{second:02d}",
                    'ltp': ltp, 'bid0_p': bid0_p, 'ask0_p': ask0_p,
                    'upper_c': upper_c, 'dist_pct': dist_to_upper_pct
                })

            # Collect Day 27 ZOMATO ticks
            if day_idx == 27 and symbol == "ZOMATO":
                day_27_zomato_ticks.append({
                    'line': line_no, 'time': f"{hour:02d}:{minute:02d}:{second:02d}",
                    'ltp': ltp, 'bid0_p': bid0_p, 'ask0_p': ask0_p,
                    'lower_c': lower_c, 'dist_pct': dist_to_lower_pct
                })

    print(f"\n[1] DATASET OVERVIEW:")
    print(f"    Total ticks processed: {total_rows:,}")
    print(f"    Alert rows processed: {alert_rows:,}")
    print(f"    Unique trading dates: {len(unique_dates)} ({unique_dates[0]} to {unique_dates[-1]})")

    print(f"\n[2] PACING & INTER-TICK GAPS ANALYSIS:")
    print(f"    Max standard intraday gap: {max_gap_info.get('gap_s', 0.0):.4f}s")
    print(f"    Max standard gap occurred at: Line {max_gap_info.get('line')}, Day {max_gap_info.get('day_idx')} ({max_gap_info.get('date')} {max_gap_info.get('time')}) on {max_gap_info.get('symbol')}")
    print(f"    Gaps > 3.0s count: {len(gaps_above_3s)}")
    for g in gaps_above_3s:
        print(f"      -> Line {g['line']}: Day {g['day_idx']} ({g['date']} {g['time']}) gap={g['gap_s']:.3f}s symbol={g['symbol']}")

    # Verification of Pacing requirement:
    # "Verify that it does not exceed 3.0 seconds (except on intentional test pauses)"
    pacing_passed = True
    if len(gaps_above_3s) == 1:
        g = gaps_above_3s[0]
        if g['day_idx'] == 22 and 5.0 < g['gap_s'] <= 8.0:
            print(f"    [PASS] Exactly 1 intentional watchdog pause found on Day 22 ({g['gap_s']:.4f}s pause at {g['time']} > 5.0s watchdog limit).")
        else:
            print(f"    [FAIL] Unexpected gap > 3.0s on Day {g['day_idx']}: {g['gap_s']}s")
            pacing_passed = False
    elif len(gaps_above_3s) == 0:
        print(f"    [WARN] Zero gaps > 3.0s found; intentional pause absent?")
        pacing_passed = False
    else:
        print(f"    [FAIL] Multiple unexpected gaps > 3.0s found ({len(gaps_above_3s)})!")
        pacing_passed = False

    if max_gap_info.get('gap_s', 0.0) <= 3.0:
        print(f"    [PASS] All non-pause intraday gaps strictly <= 3.0s (Max = {max_gap_info.get('gap_s', 0.0):.4f}s).")
    else:
        print(f"    [FAIL] Standard intraday gap exceeds 3.0s!")
        pacing_passed = False

    print(f"\n[3] CIRCUIT LIMITS & PROXIMITY AUDIT:")
    print(f"    Circuit violations (price outside [lower_c, upper_c]): {len(circuit_violations)}")
    if circuit_violations:
        for cv in circuit_violations[:10]:
            print(f"      [VIOLATION] Line {cv[0]} Day {cv[1]} ({cv[2]}): {cv[3]} {cv[4]}={cv[5]} outside [{cv[6]}, {cv[7]}]")
    else:
        print(f"    [PASS] 100% of price fields (LTP, bid0, ask0) remain strictly within [lower_c, upper_c].")

    print(f"    Zero or inverted circuits: {len(zero_or_inverted_circuits)}")
    if zero_or_inverted_circuits:
        print(f"      [FAIL] Found {len(zero_or_inverted_circuits)} invalid circuit bands!")
    else:
        print(f"    [PASS] 100% of rows have valid circuit bands (upper_c > lower_c > 0).")

    # Day 8 COALINDIA Upper Circuit Proximity Test
    print(f"\n[4] DAY 8 COALINDIA UPPER CIRCUIT PROXIMITY TEST:")
    if day_8_coalindia_ticks:
        min_dist_day8 = min(t['dist_pct'] for t in day_8_coalindia_ticks)
        closest_ticks_day8 = [t for t in day_8_coalindia_ticks if t['dist_pct'] == min_dist_day8]
        max_price_day8 = max(t['ltp'] for t in day_8_coalindia_ticks)
        upper_c_day8 = day_8_coalindia_ticks[0]['upper_c']
        print(f"    COALINDIA Upper Circuit: INR {upper_c_day8:.2f}")
        print(f"    Max LTP reached on Day 8: INR {max_price_day8:.2f}")
        print(f"    Closest distance to Upper Circuit: {min_dist_day8:.4f}%")
        print(f"    Sample closest tick: Line {closest_ticks_day8[0]['line']} at {closest_ticks_day8[0]['time']} (LTP={closest_ticks_day8[0]['ltp']}, Bid={closest_ticks_day8[0]['bid0_p']}, Ask={closest_ticks_day8[0]['ask0_p']})")
        # Check if it satisfies proximity (within 1.5% buffer, typically 1.3%)
        if min_dist_day8 <= 1.5 and max_price_day8 <= upper_c_day8:
            print(f"    [PASS] Day 8 COALINDIA successfully reached circuit proximity ({min_dist_day8:.2f}% <= 1.50% buffer) without breaching upper limit.")
            day8_passed = True
        else:
            print(f"    [FAIL] Day 8 COALINDIA did not reach expected circuit proximity ({min_dist_day8:.2f}%) or breached.")
            day8_passed = False
    else:
        print(f"    [FAIL] No Day 8 COALINDIA ticks found!")
        day8_passed = False

    # Day 27 ZOMATO Lower Circuit Proximity Test
    print(f"\n[5] DAY 27 ZOMATO LOWER CIRCUIT PROXIMITY TEST:")
    if day_27_zomato_ticks:
        min_dist_day27 = min(t['dist_pct'] for t in day_27_zomato_ticks)
        closest_ticks_day27 = [t for t in day_27_zomato_ticks if t['dist_pct'] == min_dist_day27]
        min_price_day27 = min(t['ltp'] for t in day_27_zomato_ticks)
        lower_c_day27 = day_27_zomato_ticks[0]['lower_c']
        print(f"    ZOMATO Lower Circuit: INR {lower_c_day27:.2f}")
        print(f"    Min LTP reached on Day 27: INR {min_price_day27:.2f}")
        print(f"    Closest distance to Lower Circuit: {min_dist_day27:.4f}%")
        print(f"    Sample closest tick: Line {closest_ticks_day27[0]['line']} at {closest_ticks_day27[0]['time']} (LTP={closest_ticks_day27[0]['ltp']}, Bid={closest_ticks_day27[0]['bid0_p']}, Ask={closest_ticks_day27[0]['ask0_p']})")
        if min_dist_day27 <= 1.5 and min_price_day27 >= lower_c_day27:
            print(f"    [PASS] Day 27 ZOMATO successfully reached circuit proximity ({min_dist_day27:.2f}% <= 1.50% buffer) without breaching lower limit.")
            day27_passed = True
        else:
            print(f"    [FAIL] Day 27 ZOMATO did not reach expected circuit proximity ({min_dist_day27:.2f}%) or breached.")
            day27_passed = False
    else:
        print(f"    [FAIL] No Day 27 ZOMATO ticks found!")
        day27_passed = False

    overall_circuit_passed = (len(circuit_violations) == 0 and len(zero_or_inverted_circuits) == 0 and day8_passed and day27_passed)

    print("\n" + "=" * 70)
    print(" SUMMARY ASSESSMENT:")
    print(f"  - Pacing & Gap Monotonicity: {'PASS' if pacing_passed else 'FAIL'}")
    print(f"  - Circuit Limits & Proximity: {'PASS' if overall_circuit_passed else 'FAIL'}")
    print("=" * 70)

    return pacing_passed and overall_circuit_passed

if __name__ == "__main__":
    success = run_adversarial_pacing_and_circuit_check()
    sys.exit(0 if success else 1)
