#!/usr/bin/env python3
"""
Empirical Engine Feed Parsing Verification for Milestone 1.
Slices 5,000-line chunks across diverse market regimes and runs them
through build/deltauno.exe to detect segfaults, buffer overflows, or exceptions.
Author: challenger_m1_2
"""

import os
import subprocess
import sys

DATASET = "data/historical_30days_nse.csv"
ENGINE = "build/deltauno.exe"
HEADER = "#timestamp_ns,token,symbol,last_price_inr,last_qty,volume,bid0_p,bid0_q,ask0_p,ask0_q,upper_c,lower_c\n"

TEST_SLICES = [
    ("slice_head_5k.csv", 2, 5001, "Day 1 Head & Initial Breakout"),
    ("slice_rollover_5k.csv", 17000, 22000, "Day 1 to Day 2 Session Rollover Boundary"),
    ("slice_day8_circuit_5k.csv", 134000, 139000, "Day 8 COALINDIA Upper Circuit Proximity"),
    ("slice_day22_watchdog_5k.csv", 401000, 406000, "Day 22 Watchdog Heartbeat 7.4s Pause"),
    ("slice_day27_circuit_5k.csv", 494000, 499000, "Day 27 ZOMATO Lower Circuit Proximity"),
]

def run_slice_tests():
    print("=" * 70)
    print(" EMPIRICAL CHALLENGER: C++ ENGINE FEED PARSING & MEMORY INTEGRITY")
    print(f" Target Executable: {ENGINE}")
    print(f" Source Dataset:    {DATASET}")
    print("=" * 70)

    if not os.path.exists(ENGINE):
        print(f"[FAIL] Engine executable not found: {ENGINE}")
        return False
    if not os.path.exists(DATASET):
        print(f"[FAIL] Dataset not found: {DATASET}")
        return False

    all_passed = True

    # Pre-read lines for fast slicing
    print("[*] Loading line index from dataset...")
    # Slices are around line 2 to 500,000. Let's extract each slice directly
    for filename, start_line, end_line, desc in TEST_SLICES:
        slice_path = os.path.join("data", filename)
        line_count = 0
        with open(DATASET, "r", encoding="utf-8") as f_in, open(slice_path, "w", encoding="utf-8") as f_out:
            f_out.write(HEADER)
            for curr_lno, line in enumerate(f_in, start=1):
                if curr_lno < start_line:
                    continue
                if curr_lno >= end_line:
                    break
                f_out.write(line)
                line_count += 1

        print(f"\n[*] Sliced {line_count:,} lines ({desc}) -> {slice_path}")

        # Execute DeltaUno
        cmd = [ENGINE, "--csv", slice_path]
        try:
            res = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=30
            )

            # Check exit code
            if res.returncode != 0:
                print(f"    [FAIL] Process exited with non-zero code {res.returncode}!")
                print(f"    STDERR:\n{res.stderr}")
                all_passed = False
            else:
                # Analyze output for errors or exceptions
                out = res.stdout + res.stderr
                has_crash_keywords = any(kw in out.lower() for kw in [
                    "segmentation fault", "core dumped", "buffer overflow",
                    "exception", "abort", "fatal", "terminate called"
                ])
                if has_crash_keywords:
                    print(f"    [FAIL] Crash/Exception keyword detected in output!")
                    print(f"    OUTPUT:\n{out[:500]}")
                    all_passed = False
                else:
                    print(f"    [PASS] Exit code 0 | Clean execution | Zero exceptions/faults")
                    # Check if trades or ticks were processed
                    if "Feed replay complete" in out:
                        print(f"    [PASS] Clean feed replay & shutdown confirmed.")
                    else:
                        print(f"    [WARN] 'Feed replay complete' not found in stdout.")

        except subprocess.TimeoutExpired:
            print(f"    [FAIL] Process timed out after 30 seconds!")
            all_passed = False
        except Exception as e:
            print(f"    [FAIL] Subprocess error: {e}")
            all_passed = False
        finally:
            # Clean up slice file
            if os.path.exists(slice_path):
                os.remove(slice_path)

    print("\n" + "=" * 70)
    print(f" FINAL FEED PARSING VERDICT: {'APPROVE (100% PASS)' if all_passed else 'REJECT'}")
    print("=" * 70)
    return all_passed

if __name__ == "__main__":
    success = run_slice_tests()
    sys.exit(0 if success else 1)
