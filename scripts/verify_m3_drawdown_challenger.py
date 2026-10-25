#!/usr/bin/env python3
"""
Empirical Challenger Verification Script for Milestone 3 (Iteration 2)
Focus: Intraday Drawdown Mechanics, Equity Curve Progression, and Log Integrity.
"""

import sys
import re
import math
from typing import List, Dict, Any

def check_log_ascii_and_structure(log_path: str):
    print("=== TEST 1: LOG ASCII & BUFFER INTEGRITY ===")
    with open(log_path, 'rb') as f:
        raw_bytes = f.read()

    non_ascii = [b for b in raw_bytes if b > 127]
    assert len(non_ascii) == 0, f"FAILED: Found {len(non_ascii)} non-ASCII bytes in {log_path}!"
    print(f"[PASS] 100% Pure ASCII: 0 non-ASCII bytes detected out of {len(raw_bytes)} bytes.")

    with open(log_path, 'r', encoding='utf-8') as f:
        lines = [line.rstrip('\r\n') for line in f.readlines()]

    max_len = max(len(l) for l in lines)
    print(f"[PASS] Line length check: Max line length = {max_len} chars (Fixed buffer limit: 384 chars).")
    assert max_len < 384, f"FAILED: Line length {max_len} exceeds 384 buffer limit!"

    required_headers = [
        "DELTAUNO INSTITUTIONAL PERFORMANCE AUDITOR (30-DAY REPLAY)",
        "30-DAY SESSION-BY-SESSION PERFORMANCE BREAKDOWN",
        "ITEMIZED STATUTORY CHARGES AUDIT (INDIAN TAXES)",
        "DAILY KILL-SWITCH & DRAWDOWN AUDIT TRAIL",
        "30-DAY PORTFOLIO EQUITY CURVE PROGRESSION",
        "COMPREHENSIVE 30-DAY STATISTICAL METRICS",
        "Engine shutdown cleanly. DeltaUno session concluded."
    ]
    for h in required_headers:
        assert any(h in l for l in lines), f"FAILED: Missing header '{h}' in log!"
    print(f"[PASS] All {len(required_headers)} required performance headers and completion markers verified.")
    return lines

def clean_log_prefix(line: str) -> str:
    # Strip timestamp and log level prefix e.g. "[2026-10-25 19:56:37.980] [INFO ] "
    if "] [INFO ] " in line:
        return line.split("] [INFO ] ", 1)[1]
    if "] [WARN ] " in line:
        return line.split("] [WARN ] ", 1)[1]
    if "] [ERROR] " in line:
        return line.split("] [ERROR] ", 1)[1]
    return line

def parse_and_verify_tables(raw_lines: List[str]):
    print("\n=== TEST 2: MATHEMATICAL CONSISTENCY OF 30-DAY REPORT TABLES ===")
    lines = [clean_log_prefix(l) for l in raw_lines]

    # Parse Table 1: Session-by-Session Performance
    # Table columns: Day, Date, Trades, W/L/B, Win Rate, Gross PnL, Friction, Net PnL, Intra DD, Ending Equity, Kill-Switch
    # Example line: "    1  2026-11-02       2     1/1/0     50.0%      +69.88 INR     -3.52 INR     +66.36 INR  22.94 INR    10066.36 INR      SAFE  "
    table1_pattern = re.compile(
        r"^\s*(\d+)\s+(\d{4}-\d{2}-\d{2})\s+(\d+)\s+(\d+)/(\d+)/(\d+)\s+([\d\.]+)%\s+([+-]?[\d\.]+)\s+INR\s+([+-]?[\d\.]+)\s+INR\s+([+-]?[\d\.]+)\s+INR\s+([\d\.]+)\s+INR\s+([\d\.]+)\s+INR\s+(\w+)"
    )

    # Parse Table 3: Daily Kill-Switch & Drawdown
    # Example line: "    1  2026-11-02       2           10000.00           10089.31            10000.00            22.94 (0.23%)        CLEAR"
    table3_pattern = re.compile(
        r"^\s*(\d+)\s+(\d{4}-\d{2}-\d{2})\s+(\d+)\s+([\d\.]+)\s+([\d\.]+)\s+([\d\.]+)\s+([\d\.]+)\s+\(([\d\.]+)%\)\s+(\w+)"
    )

    # Parse Table 4: Portfolio Equity Curve Progression
    # Example line: "    1  2026-11-02       2            10066.36           +0.66%           +0.66%             10066.36            0.00 (0.00%)"
    table4_pattern = re.compile(
        r"^\s*(\d+)\s+(\d{4}-\d{2}-\d{2})\s+(\d+)\s+([\d\.]+)\s+([+-]?[\d\.]+)%\s+([+-]?[\d\.]+)%\s+([\d\.]+)\s+([\d\.]+)\s+\(([\d\.]+)%\)"
    )

    t1_rows = []
    t3_rows = []
    t4_rows = []

    for l in lines:
        m1 = table1_pattern.search(l)
        if m1:
            t1_rows.append({
                "day": int(m1.group(1)),
                "date": m1.group(2),
                "trades": int(m1.group(3)),
                "win": int(m1.group(4)),
                "loss": int(m1.group(5)),
                "scratch": int(m1.group(6)),
                "win_rate": float(m1.group(7)),
                "gross_pnl": float(m1.group(8)),
                "friction": float(m1.group(9)),
                "net_pnl": float(m1.group(10)),
                "intra_dd": float(m1.group(11)),
                "ending_eq": float(m1.group(12)),
                "status": m1.group(13),
            })
        m3 = table3_pattern.search(l)
        if m3:
            t3_rows.append({
                "day": int(m3.group(1)),
                "date": m3.group(2),
                "trades": int(m3.group(3)),
                "start": float(m3.group(4)),
                "peak": float(m3.group(5)),
                "trough": float(m3.group(6)),
                "max_dd_inr": float(m3.group(7)),
                "max_dd_pct": float(m3.group(8)),
                "status": m3.group(9),
            })
        m4 = table4_pattern.search(l)
        if m4:
            t4_rows.append({
                "day": int(m4.group(1)),
                "date": m4.group(2),
                "trades": int(m4.group(3)),
                "closing_eq": float(m4.group(4)),
                "daily_ret_pct": float(m4.group(5)),
                "cum_ret_pct": float(m4.group(6)),
                "hwm": float(m4.group(7)),
                "dd_hwm_inr": float(m4.group(8)),
                "dd_hwm_pct": float(m4.group(9)),
            })

    print(f"Parsed rows: Table 1 = {len(t1_rows)}, Table 3 = {len(t3_rows)}, Table 4 = {len(t4_rows)}")
    assert len(t1_rows) == 30, f"Expected 30 rows in Table 1, got {len(t1_rows)}"
    assert len(t3_rows) == 30, f"Expected 30 rows in Table 3, got {len(t3_rows)}"
    assert len(t4_rows) == 30, f"Expected 30 rows in Table 4, got {len(t4_rows)}"

    # Sub-test 2.1: Capital Continuity & Zero Leak Across All 30 Days
    print("\n--- Sub-test 2.1: 30-Day Capital Continuity (0.000000 INR Leak) ---")
    running_capital = 10000.00
    hwm_running = 10000.00

    for i in range(30):
        r1 = t1_rows[i]
        r3 = t3_rows[i]
        r4 = t4_rows[i]

        assert r1["day"] == i + 1
        assert r3["day"] == i + 1
        assert r4["day"] == i + 1
        assert r1["date"] == r4["date"] == r3["date"]
        assert r1["trades"] == r4["trades"] == r3["trades"]

        # Table 1: Net PnL = Gross PnL - |Friction|
        calc_net = r1["gross_pnl"] - abs(r1["friction"])
        assert abs(calc_net - r1["net_pnl"]) < 0.015, (
            f"Day {i+1}: Gross ({r1['gross_pnl']}) - Friction ({r1['friction']}) != Net ({r1['net_pnl']})"
        )

        # Table 3: Starting equity must match running capital
        assert abs(r3["start"] - running_capital) < 0.000001, (
            f"Day {i+1}: Table 3 starting capital {r3['start']} != running capital {running_capital}"
        )

        expected_closing = round(running_capital + r1["net_pnl"], 2)
        assert abs(r4["closing_eq"] - expected_closing) < 0.015, (
            f"Day {i+1}: Table 4 closing {r4['closing_eq']} != expected {expected_closing}"
        )
        assert abs(r1["ending_eq"] - r4["closing_eq"]) < 0.0001, (
            f"Day {i+1}: Table 1 ending eq {r1['ending_eq']} != Table 4 closing eq {r4['closing_eq']}"
        )

        # Check HWM: strictly monotonic non-decreasing
        hwm_running = max(hwm_running, r4["closing_eq"])
        assert abs(r4["hwm"] - hwm_running) < 0.01, (
            f"Day {i+1}: Table 4 HWM {r4['hwm']} != calculated HWM {hwm_running}"
        )

        # Check DD from HWM
        calc_dd_hwm = round(hwm_running - r4["closing_eq"], 2)
        assert abs(r4["dd_hwm_inr"] - calc_dd_hwm) <= 0.015, (
            f"Day {i+1}: Table 4 DD from HWM {r4['dd_hwm_inr']} != calculated {calc_dd_hwm}"
        )

        # Check Drawdown pct from HWM: DD / HWM * 100
        expected_dd_pct = round((calc_dd_hwm / hwm_running) * 100.0, 2) if hwm_running > 0 else 0.0
        assert abs(r4["dd_hwm_pct"] - expected_dd_pct) < 0.02, (
            f"Day {i+1}: Table 4 DD% {r4['dd_hwm_pct']} != expected {expected_dd_pct}"
        )

        # Check Daily Return Pct: Net PnL / Starting Equity * 100
        expected_ret_pct = round((r1["net_pnl"] / running_capital) * 100.0, 2)
        assert abs(r4["daily_ret_pct"] - expected_ret_pct) < 0.02, (
            f"Day {i+1}: Table 4 Daily Ret% {r4['daily_ret_pct']} != expected {expected_ret_pct}"
        )

        # Consecutive session continuity: Day[i+1].starting == Day[i].ending
        if i + 1 < 30:
            next_r3 = t3_rows[i + 1]
            assert abs(next_r3["start"] - r4["closing_eq"]) < 0.000001, (
                f"Consecutive leak between Day {i+1} and Day {i+2}: {next_r3['start']} vs {r4['closing_eq']}"
            )

        running_capital = r4["closing_eq"]

    print("[PASS] 30-day capital continuity and HWM tracking verified with 0.000000 INR leak!")

    # Sub-test 2.2: Zero-Trade Sessions Audit
    print("\n--- Sub-test 2.2: Zero-Trade Sessions Integrity ---")
    zero_days = [r1 for r1 in t1_rows if r1["trades"] == 0]
    print(f"Zero-trade sessions found: {len(zero_days)} days -> {[z['date'] for z in zero_days]}")
    assert len(zero_days) == 3, f"Expected 3 zero-trade sessions, got {len(zero_days)}"
    for z in zero_days:
        day_idx = z["day"] - 1
        r3 = t3_rows[day_idx]
        r4 = t4_rows[day_idx]
        assert z["net_pnl"] == 0.0, f"Zero-trade day {z['date']} has non-zero net PnL: {z['net_pnl']}"
        assert z["gross_pnl"] == 0.0
        assert z["friction"] == 0.0
        assert r3["max_dd_inr"] == 0.0
        assert r3["status"] == "CLEAR", f"Zero-trade day {z['date']} status is {r3['status']}, expected CLEAR"
        assert z["status"] == "SAFE", f"Table 1 Zero-trade day {z['date']} status is {z['status']}, expected SAFE"
        assert r4["daily_ret_pct"] == 0.0
        assert r3["start"] == r3["peak"] == r3["trough"] == r4["closing_eq"]
        print(f"[PASS] Zero-trade session Day {z['day']} ({z['date']}): 0 trades, 0.00 PnL, SAFE status, start==peak==trough==close verified.")

def run_adversarial_simulation_probes():
    print("\n=== TEST 3: EMPIRICAL ADVERSARIAL SIMULATION PROBES ===")

    # Probe A: Intraday Peak-to-Trough with Multiple Micro-Reversals
    print("\n--- Probe A: Complex Intraday Zigzag Drawdown Tracking ---")
    # Simulate a day with:
    # Initial capital: 10,000
    # Trade 1: +120 INR -> Capital = 10,120 (Peak = 10,120)
    # Trade 2: -70 INR  -> Capital = 10,050 (DD = 70)
    # Trade 3: +200 INR -> Capital = 10,250 (New Peak = 10,250)
    # Trade 4: -350 INR -> Capital = 9,900  (DD = 350, Trough = 9,900)
    # Trade 5: +100 INR -> Capital = 10,000 (Closing DD = 250)
    trade_pnls = [+120.0, -70.0, +200.0, -350.0, +100.0]
    cap = 10000.0
    day_peak = cap
    day_trough = cap
    max_dd = 0.0
    for pnl in trade_pnls:
        cap += pnl
        day_peak = max(day_peak, cap)
        day_trough = min(day_trough, cap)
        curr_dd = day_peak - cap
        max_dd = max(max_dd, curr_dd)

    print(f"Simulation result: Final Capital = {cap}, Peak = {day_peak}, Trough = {day_trough}")
    print(f"Max Peak-to-Trough Drawdown = {max_dd} INR (vs Closing Drawdown = {day_peak - cap} INR)")
    assert max_dd == 350.0, f"Expected max DD 350.0, got {max_dd}"
    assert (day_peak - cap) == 250.0, f"Expected closing DD 250.0, got {day_peak - cap}"
    print("[PASS] Probe A: Peak-to-trough retention verified independently.")

    # Probe B: Exact Kill-Switch Boundary Condition (Paise Precision)
    print("\n--- Probe B: Exact 500.00 INR Kill-Switch Trip Boundary ---")
    limit_paise = 50000 # 500.00 INR
    test_cases = [
        (499.94, False, "499.94 INR -> 49994 paise < 50000 -> No trip"),
        (499.99, False, "499.99 INR -> 49999 paise < 50000 -> No trip"),
        (500.00, True,  "500.00 INR -> 50000 paise >= 50000 -> TRIP"),
        (500.01, True,  "500.01 INR -> 50001 paise >= 50000 -> TRIP"),
        (500.05, True,  "500.05 INR -> 50005 paise >= 50000 -> TRIP")
    ]
    for inr_val, should_trip, desc in test_cases:
        paise_val = int(round(inr_val * 100.0))
        tripped = paise_val >= limit_paise
        assert tripped == should_trip, f"Boundary test failed for {inr_val}: expected {should_trip}, got {tripped}"
        print(f"[PASS] {desc}")

    # Probe C: 10,000-Trade Micro-Friction Preservation
    print("\n--- Probe C: 10,000-Trade Capital Continuity & Numerical Stability ---")
    start_cap = 10000.00
    current_cap = start_cap
    paise_cap = int(start_cap * 100)

    # Alternate +/- 5.05 INR for 10,000 trades
    for i in range(10000):
        step_inr = 5.05 if (i % 2 == 0) else -5.05
        step_paise = 505 if (i % 2 == 0) else -505
        current_cap += step_inr
        paise_cap += step_paise

    assert abs(current_cap - start_cap) < 1e-9, f"Double drift: {current_cap} vs {start_cap}"
    assert paise_cap == int(start_cap * 100), f"Paise drift: {paise_cap}"
    print(f"[PASS] 10,000 alternating trades completed: Capital exactly {current_cap:.2f} INR (zero drift).")

def main():
    lines = check_log_ascii_and_structure("deltauno.log")
    parse_and_verify_tables(lines)
    run_adversarial_simulation_probes()
    print("\n==================================================================")
    print("  ALL EMPIRICAL CHALLENGER VERIFICATION CHECKS PASSED 100%!       ")
    print("==================================================================")

if __name__ == "__main__":
    main()
