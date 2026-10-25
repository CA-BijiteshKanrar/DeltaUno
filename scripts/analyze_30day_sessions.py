#!/usr/bin/env python3
"""
Multi-Day Rollover Boundary Analysis across data/historical_30days_nse.csv
Author: challenger_m2_2
"""
import sys
import datetime

CSV_PATH = "data/historical_30days_nse.csv"

def analyze_sessions():
    print("=" * 80)
    print("EMPIRICAL CHALLENGER: 30-DAY HISTORICAL NSE SESSION BOUNDARY ANALYSIS")
    print(f"Dataset: {CSV_PATH}")
    print("=" * 80)

    days = {}  # ist_day_index -> {'date': ..., 'first_ts': ..., 'last_ts': ..., 'tick_count': ...}
    day_order = []

    total_ticks = 0
    with open(CSV_PATH, "r", encoding="utf-8") as f:
        header = f.readline()
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            ts_ns = int(parts[0])
            ts_sec = ts_ns / 1e9
            ist_day = int((ts_sec + 19800) // 86400)

            total_ticks += 1
            if ist_day not in days:
                dt_utc = datetime.datetime.fromtimestamp(ts_sec, datetime.timezone.utc)
                dt_ist = dt_utc + datetime.timedelta(hours=5, minutes=30)
                date_str = dt_ist.strftime("%Y-%m-%d")
                days[ist_day] = {
                    'date': date_str,
                    'first_ts': ts_ns,
                    'last_ts': ts_ns,
                    'first_dt_ist': dt_ist,
                    'last_dt_ist': dt_ist,
                    'tick_count': 1
                }
                day_order.append(ist_day)
            else:
                days[ist_day]['last_ts'] = ts_ns
                dt_utc = datetime.datetime.fromtimestamp(ts_sec, datetime.timezone.utc)
                dt_ist = dt_utc + datetime.timedelta(hours=5, minutes=30)
                days[ist_day]['last_dt_ist'] = dt_ist
                days[ist_day]['tick_count'] += 1

    print(f"Total Ticks Read: {total_ticks:,}")
    print(f"Unique Trading Days: {len(day_order)}")
    print("-" * 80)
    print(f"{'Day #':<6} {'IST Date':<12} {'IST Day Index':<14} {'Start IST':<16} {'End IST':<16} {'Ticks':<10}")
    print("-" * 80)

    for i, ist_day in enumerate(day_order, start=1):
        info = days[ist_day]
        start_str = info['first_dt_ist'].strftime("%H:%M:%S.%f")[:12]
        end_str = info['last_dt_ist'].strftime("%H:%M:%S.%f")[:12]
        print(f"{i:<6} {info['date']:<12} {ist_day:<14} {start_str:<16} {end_str:<16} {info['tick_count']:<10,}")

    print("=" * 80)
    print("SESSION ROLLOVER BOUNDARIES (Day N 15:30:00 -> Day N+1 09:15:00 IST)")
    print("=" * 80)
    print(f"{'Boundary #':<12} {'From Date (Day N)':<18} {'End Time IST':<15} {'To Date (Day N+1)':<18} {'Start Time IST':<15} {'Gap (Hours)':<12} {'Clean?':<8}")
    print("-" * 80)

    rollover_errors = 0
    gap_summary = []

    for i in range(len(day_order) - 1):
        day_n = day_order[i]
        day_n1 = day_order[i + 1]

        info_n = days[day_n]
        info_n1 = days[day_n1]

        end_n = info_n['last_dt_ist']
        start_n1 = info_n1['first_dt_ist']

        gap_sec = (info_n1['first_ts'] - info_n['last_ts']) / 1e9
        gap_hours = gap_sec / 3600.0
        gap_summary.append((i + 1, gap_hours, gap_sec))

        # Verification: Day N ends at market close (15:29:50 - 15:30:00 IST), Day N+1 starts at 09:15:00 IST
        is_clean_end = (end_n.hour == 15 and end_n.minute == 29 and end_n.second >= 50) or (end_n.hour == 15 and end_n.minute == 30 and end_n.second == 0)
        is_clean_start = (start_n1.hour == 9 and start_n1.minute == 15 and start_n1.second == 0)

        clean = is_clean_end and is_clean_start
        if not clean:
            rollover_errors += 1

        print(f"Boundary {i+1:<3} {info_n['date']} (D{i+1:<2})  {end_n.strftime('%H:%M:%S'):<15} {info_n1['date']} (D{i+2:<2})  {start_n1.strftime('%H:%M:%S'):<15} {gap_hours:8.2f}h     {'PASS' if clean else 'FAIL'}")

    print("=" * 80)
    print(f"Total Rollover Boundaries Evaluated: {len(day_order) - 1}")
    print(f"Clean Boundaries (15:30:00 -> 09:15:00): {len(day_order) - 1 - rollover_errors}")
    print(f"Boundary Violations: {rollover_errors}")

    min_gap = min(g[1] for g in gap_summary)
    max_gap = max(g[1] for g in gap_summary)
    print(f"Overnight Gap Range: min={min_gap:.2f}h, max={max_gap:.2f}h (weekends extend gap to ~65.75h)")
    print("=" * 80)

    return rollover_errors == 0 and len(day_order) == 30

if __name__ == "__main__":
    success = analyze_sessions()
    sys.exit(0 if success else 1)
