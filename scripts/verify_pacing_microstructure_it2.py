#!/usr/bin/env python3
"""
Empirical Challenger Verification Script for Milestone 1 Iteration 2
Auditing:
1. Inter-tick pacing gaps (<= 3.0s during sessions, exactly one 6.0s pause on Day 22 @ 10:30 IST).
2. Calendar completeness (30 unique trading sessions 09:15-15:30 IST, all 5 symbols continuous).
3. Alert regimes:
   - Circuit proximity days (Day 10, Day 25): check if they enter circuit buffer (< 1.5% from circuit).
   - Also scan all 30 days for circuit proximity (< 1.5%).
   - Macro-news days (Day 5, Day 14): evaluate depth and check for depth dry-up.
   - Inspect all embedded #ALERT lines.
"""

import sys
import os
import datetime
from collections import defaultdict

CSV_PATH = "data/historical_30days_nse.csv"

def ns_to_ist(ts_ns: int):
    total_sec = ts_ns // 1_000_000_000
    ist_sec = total_sec + 19800
    sec_of_day = ist_sec % 86400
    h = sec_of_day // 3600
    m = (sec_of_day % 3600) // 60
    s = sec_of_day % 60
    dt = datetime.datetime.fromtimestamp(total_sec, tz=datetime.timezone.utc) + datetime.timedelta(hours=5, minutes=30)
    return dt.strftime("%Y-%m-%d"), h, m, s, sec_of_day

def audit():
    if not os.path.exists(CSV_PATH):
        print(f"ERROR: {CSV_PATH} not found")
        sys.exit(1)

    print("=" * 80)
    print("EMPIRICAL PACING, CALENDAR & MICROSTRUCTURE AUDIT (M1 IT2)")
    print(f"Target: {CSV_PATH}")
    print("=" * 80)

    total_rows = 0
    alerts = []
    
    unique_dates = []
    date_to_day_idx = {}
    
    # Per-day, per-symbol stats
    day_symbol_ticks = defaultdict(lambda: defaultdict(list)) # date -> symbol -> list of ts_ns
    day_symbol_depth = defaultdict(lambda: defaultdict(list)) # date -> symbol -> list of (bid_q, ask_q, total_depth)
    day_min_circuit_dist = defaultdict(lambda: defaultdict(lambda: 999.0)) # date -> symbol -> min_dist_pct
    
    # Inter-tick pacing tracking (across all symbols chronologically)
    prev_ts_ns = 0
    prev_date = ""
    
    gaps_gt_3s = []
    all_intraday_gaps = []
    
    # Circuit violations
    circuit_breaches = []
    
    with open(CSV_PATH, "r", encoding="utf-8") as f:
        header = f.readline().strip()
        
        for line_no, raw_line in enumerate(f, start=2):
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("#ALERT,"):
                alerts.append((line_no, line))
                continue
            
            total_rows += 1
            parts = line.split(",")
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
            
            date_str, h, m, s, sec_of_day = ns_to_ist(ts_ns)
            
            if date_str not in date_to_day_idx:
                unique_dates.append(date_str)
                date_to_day_idx[date_str] = len(unique_dates)
            
            day_idx = date_to_day_idx[date_str]
            
            day_symbol_ticks[date_str][symbol].append(sec_of_day)
            day_symbol_depth[date_str][symbol].append((bid0_q, ask0_q, bid0_q + ask0_q, sec_of_day))
            
            # Circuit distance
            dist_u = (upper_c - ltp) / ltp * 100.0
            dist_l = (ltp - lower_c) / ltp * 100.0
            min_dist = min(dist_u, dist_l)
            if min_dist < day_min_circuit_dist[date_str][symbol]:
                day_min_circuit_dist[date_str][symbol] = min_dist
            
            if ltp > upper_c or ltp < lower_c:
                circuit_breaches.append((line_no, day_idx, date_str, symbol, ltp, lower_c, upper_c))
            
            # Pacing check (within same date, during 09:15:00 - 15:30:00 IST = 33300 - 55800)
            if prev_date == date_str and (33300 <= sec_of_day <= 55800):
                gap_s = (ts_ns - prev_ts_ns) / 1_000_000_000.0
                all_intraday_gaps.append((gap_s, line_no, day_idx, date_str, h, m, s, symbol))
                if gap_s > 3.0:
                    gaps_gt_3s.append({
                        "line": line_no,
                        "day_idx": day_idx,
                        "date": date_str,
                        "time": f"{h:02d}:{m:02d}:{s:02d}",
                        "gap_s": gap_s,
                        "symbol": symbol
                    })
            
            prev_ts_ns = ts_ns
            prev_date = date_str

    print(f"\n[SECTION 1: DATASET GENERAL STATS]")
    print(f"Total data rows: {total_rows:,}")
    print(f"Total alert rows: {len(alerts)}")
    for a in alerts:
        print(f"  Alert at line {a[0]}: {a[1]}")
    print(f"Unique trading days: {len(unique_dates)}")
    for idx, d in enumerate(unique_dates, 1):
        print(f"  Day {idx:2d}: {d} ({len(day_symbol_ticks[d])} symbols, total ticks = {sum(len(v) for v in day_symbol_ticks[d].values())})")

    print(f"\n[SECTION 2: PACING GAPS AUDIT]")
    print(f"Total intraday gaps analyzed: {len(all_intraday_gaps):,}")
    max_non_pause_gap = max((g[0] for g in all_intraday_gaps if g[0] <= 3.0), default=0.0)
    print(f"Max standard intraday gap (<= 3.0s): {max_non_pause_gap:.4f}s")
    print(f"Gaps > 3.0s count: {len(gaps_gt_3s)}")
    for g in gaps_gt_3s:
        print(f"  -> Line {g['line']}: Day {g['day_idx']} ({g['date']} {g['time']}) gap={g['gap_s']:.4f}s on {g['symbol']}")
    
    # Check intentional pause on Day 22 at 10:30 IST
    day22_pauses = [g for g in gaps_gt_3s if g['day_idx'] == 22]
    print(f"Day 22 pauses > 3.0s: {len(day22_pauses)}")
    for p in day22_pauses:
        print(f"  Day 22 pause detail: Time={p['time']}, Gap={p['gap_s']:.4f}s, Symbol={p['symbol']}")
    
    # Other pauses
    other_pauses = [g for g in gaps_gt_3s if g['day_idx'] != 22]
    print(f"Non-Day-22 pauses > 3.0s: {len(other_pauses)}")

    print(f"\n[SECTION 3: CALENDAR COMPLETENESS AUDIT]")
    # Check if all 30 days have all 5 symbols starting at 09:15:00 and ending at 15:30:00
    all_calendar_complete = True
    symbols_expected = {"TATASTEEL", "SBIN", "ITC", "COALINDIA", "ZOMATO"}
    for idx, d in enumerate(unique_dates, 1):
        syms_present = set(day_symbol_ticks[d].keys())
        missing_syms = symbols_expected - syms_present
        if missing_syms:
            print(f"  [FAIL] Day {idx} ({d}) missing symbols: {missing_syms}")
            all_calendar_complete = False
            continue
        
        # Check start and end times for each symbol
        day_starts = {}
        day_ends = {}
        for s in symbols_expected:
            ticks = day_symbol_ticks[d][s]
            first_sec = ticks[0]
            last_sec = ticks[-1]
            day_starts[s] = first_sec
            day_ends[s] = last_sec
            
            # Start should be between 33300 and 33310 (09:15:00 to 09:15:10)
            if not (33300 <= first_sec <= 33315):
                print(f"  [WARN] Day {idx} {s} start sec={first_sec} ({first_sec//3600:02d}:{(first_sec%3600)//60:02d}:{first_sec%60:02d})")
                all_calendar_complete = False
            # End should be near 55800 (15:30:00)
            if not (55780 <= last_sec <= 55810):
                print(f"  [WARN] Day {idx} {s} end sec={last_sec} ({last_sec//3600:02d}:{(last_sec%3600)//60:02d}:{last_sec%60:02d})")
                all_calendar_complete = False

    print(f"Calendar completeness check (30 sessions x 5 symbols x 09:15-15:30): {'PASS' if all_calendar_complete else 'FAIL'}")

    print(f"\n[SECTION 4: CIRCUIT PROXIMITY AUDIT]")
    print(f"Circuit limit breaches (LTP outside [lower, upper]): {len(circuit_breaches)}")
    
    print("\nMin Circuit Proximity per Day (across all 5 symbols):")
    for idx, d in enumerate(unique_dates, 1):
        min_p = min(day_min_circuit_dist[d].values())
        closest_sym = [s for s, dist in day_min_circuit_dist[d].items() if dist == min_p][0]
        near_buffer = min_p < 1.5
        flag = " [CIRCUIT BUFFER < 1.5%]" if near_buffer else ""
        print(f"  Day {idx:2d} ({d}): Closest symbol = {closest_sym} ({min_p:.3f}% from circuit){flag}")

    print(f"\nTargeted Check on Objective-specified Days: Day 10 and Day 25")
    day10_date = unique_dates[9]
    day25_date = unique_dates[24]
    print(f"  Day 10 ({day10_date}) Circuit Distances:")
    for s, dist in day_min_circuit_dist[day10_date].items():
        print(f"    {s}: {dist:.3f}% from circuit")
    print(f"  Day 25 ({day25_date}) Circuit Distances:")
    for s, dist in day_min_circuit_dist[day25_date].items():
        print(f"    {s}: {dist:.3f}% from circuit")

    print(f"\nTargeted Check on Script-configured Circuit Days: Day 8 and Day 27")
    day8_date = unique_dates[7]
    day27_date = unique_dates[26]
    print(f"  Day 8 ({day8_date}) Circuit Distances:")
    for s, dist in day_min_circuit_dist[day8_date].items():
        print(f"    {s}: {dist:.3f}% from circuit")
    print(f"  Day 27 ({day27_date}) Circuit Distances:")
    for s, dist in day_min_circuit_dist[day27_date].items():
        print(f"    {s}: {dist:.3f}% from circuit")

    print(f"\n[SECTION 5: MACRO-NEWS DAYS DEPTH DRY-UP AUDIT]")
    # Check Day 5 and Day 14 depth vs other days
    # Day 5 is FOMC freeze day (09:15-10:00 IST), Day 14 is overcrowded exit trap alert
    print("Average Top-of-Book Depth (bid0_q + ask0_q) per Day per Symbol:")
    all_days_avg_depth = defaultdict(list)
    for idx, d in enumerate(unique_dates, 1):
        print(f"  Day {idx:2d} ({d}):", end="")
        for s in sorted(symbols_expected):
            depths = [depth[2] for depth in day_symbol_depth[d][s]]
            avg_d = sum(depths) / len(depths) if depths else 0
            all_days_avg_depth[s].append((idx, avg_d))
            print(f" {s[:4]}={avg_d:.1f}", end="")
        print()

    # Specifically analyze Day 5 and Day 14
    for target_day_idx in [5, 14]:
        t_date = unique_dates[target_day_idx - 1]
        print(f"\nDeep Dive on Day {target_day_idx} ({t_date}):")
        for s in sorted(symbols_expected):
            depths = day_symbol_depth[t_date][s]
            # Check depth during macro window:
            # For Day 5: 09:15 to 10:00 IST (33300 to 36000)
            # For Day 14: 10:00 to 11:00 IST (36000 to 39600)
            window_depths = [d[2] for d in depths if (33300 <= d[3] <= 36000)]
            outside_depths = [d[2] for d in depths if not (33300 <= d[3] <= 36000)]
            avg_win = sum(window_depths)/len(window_depths) if window_depths else 0
            avg_out = sum(outside_depths)/len(outside_depths) if outside_depths else 0
            min_depth = min((d[2] for d in depths), default=0)
            print(f"  {s}: Window Avg Depth = {avg_win:.1f}, Outside Avg Depth = {avg_out:.1f}, Min Depth = {min_depth}")

if __name__ == "__main__":
    audit()
