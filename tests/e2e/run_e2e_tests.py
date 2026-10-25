#!/usr/bin/env python3
"""
DeltaUno Master E2E Test Runner
===============================
Unified test harness and orchestrator for all 4 testing tiers:
  Tier 1: Feature Coverage (80 tests across 16 features)
  Tier 2: Boundary & Corner Cases (80 tests across 16 features)
  Tier 3: Cross-Feature Combinations (16 pairwise interaction tests)
  Tier 4: Real-World Application Scenarios (8 realistic workloads)

Total Test Inventory: 184 Genuine, Opaque-Box E2E Tests.

Usage:
  python tests/e2e/run_e2e_tests.py               # Run all 184 tests
  python tests/e2e/run_e2e_tests.py --tier 1       # Run only Tier 1
  python tests/e2e/run_e2e_tests.py --tier 1,4     # Run Tiers 1 and 4
  python tests/e2e/run_e2e_tests.py --feature F09  # Run tests covering Feature 09
  python tests/e2e/run_e2e_tests.py --list         # List all tests without running
  python tests/e2e/run_e2e_tests.py -v             # Verbose execution output
"""

import argparse
import os
import sys
import time
import unittest
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

# Ensure repository root is in sys.path
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.e2e.e2e_context import (
    DATA_30DAY,
    find_engine_executable,
)

# Reconfigure stdout/stderr for UTF-8 cross-platform console stability
if sys.platform == "win32":
    try:
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        if hasattr(sys.stderr, "reconfigure"):
            sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


def sanitize_output(text: Optional[str]) -> str:
    """Sanitize docstrings and messages to pure ASCII for console safety."""
    if not text:
        return ""
    clean = text.replace("\u20b9", "INR ")
    encoding = getattr(sys.stdout, "encoding", None) or "utf-8"
    try:
        clean.encode(encoding)
        return clean
    except UnicodeEncodeError:
        return clean.encode("ascii", errors="replace").decode("ascii")


# Tier Modules Mapping
TIER_MODULES = {
    1: {
        "module": "tests.e2e.test_tier1_features",
        "name": "Tier 1: Feature Coverage",
        "desc": "Validates primary requirements across all 16 features (>=5 per feature)",
        "expected_count": 80,
    },
    2: {
        "module": "tests.e2e.test_tier2_boundaries",
        "name": "Tier 2: Boundary & Corner Cases",
        "desc": "Validates limit conditions, edge values, circuits, and fail-safes (>=5 per feature)",
        "expected_count": 80,
    },
    3: {
        "module": "tests.e2e.test_tier3_pairwise",
        "name": "Tier 3: Cross-Feature Combinations",
        "desc": "Validates pairwise multi-feature interactions, race prevention, and state integrity",
        "expected_count": 16,
    },
    4: {
        "module": "tests.e2e.test_tier4_scenarios",
        "name": "Tier 4: Real-World Application Scenarios",
        "desc": "Validates realistic production workloads, clearing reconciliation, stress bursts, macro events",
        "expected_count": 8,
    },
}

FEATURE_NAMES = {
    "F01": "Deterministic 1-Min Data Ingestion",
    "F02": "L2 Microstructure & Multi-Horizon OFI",
    "F03": "Opening Range & Momentum Breakout",
    "F04": "Slippage-Guarded Market Fill Simulation",
    "F05": "Execution Friction & Liquidity Penalties",
    "F06": "Dynamic Breakeven & Ratchet Stops",
    "F07": "Strict EOD Auto-Squareoff Shield (15:05 IST)",
    "F08": "Institutional Capital Preservation",
    "F09": "13 Institutional Risk Shields",
    "F10": "Session Day Partitioning & Rollover",
    "F11": "Statutory Indian Friction Modeling",
    "F12": "Audit Logging & Kill-Switch History",
    "F13": "Equity Curve & Drawdown Tracking",
    "F14": "Crash Recovery & JSON State Journal",
    "F15": "Realtime Pacing & Pipeline Watchdog",
    "F16": "External TradingView Signal Ingestion",
}


@dataclass
class TestRecord:
    tier: int
    test_id: str
    class_name: str
    method_name: str
    doc: str
    status: str = "PENDING"  # PASS, FAIL, ERROR, SKIP
    duration_s: float = 0.0
    error_message: str = ""


class DetailedTestResult(unittest.TestResult):
    """Custom TestResult collecting detailed execution records per test."""

    def __init__(self, tier: int, verbose: bool = False):
        super().__init__()
        self.tier = tier
        self.verbose = verbose
        self.records: List[TestRecord] = []
        self._start_time = 0.0

    def startTest(self, test):
        super().startTest(test)
        self._start_time = time.perf_counter()
        if self.verbose:
            doc = (test.shortDescription() or "").strip()
            print(f"  [{test.id().split('.')[-1]}] {sanitize_output(doc)} ... ", end="", flush=True)

    def addSuccess(self, test):
        super().addSuccess(test)
        elapsed = time.perf_counter() - self._start_time
        rec = self._make_record(test, "PASS", elapsed)
        self.records.append(rec)
        if self.verbose:
            print(f"\033[92mPASS\033[0m ({elapsed:.3f}s)")

    def addFailure(self, test, err):
        super().addFailure(test, err)
        elapsed = time.perf_counter() - self._start_time
        rec = self._make_record(test, "FAIL", elapsed, self._exc_info_to_string(err, test))
        self.records.append(rec)
        if self.verbose:
            print(f"\033[91mFAIL\033[0m ({elapsed:.3f}s)")

    def addError(self, test, err):
        super().addError(test, err)
        elapsed = time.perf_counter() - self._start_time
        rec = self._make_record(test, "ERROR", elapsed, self._exc_info_to_string(err, test))
        self.records.append(rec)
        if self.verbose:
            print(f"\033[91mERROR\033[0m ({elapsed:.3f}s)")

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        elapsed = time.perf_counter() - self._start_time
        rec = self._make_record(test, "SKIP", elapsed, reason)
        self.records.append(rec)
        if self.verbose:
            print(f"\033[93mSKIP\033[0m ({reason})")

    def _make_record(self, test, status: str, duration: float, error_msg: str = "") -> TestRecord:
        parts = test.id().split(".")
        class_name = parts[-2] if len(parts) >= 2 else ""
        method_name = parts[-1] if len(parts) >= 1 else ""
        doc = (test.shortDescription() or "").strip()
        return TestRecord(
            tier=self.tier,
            test_id=test.id(),
            class_name=class_name,
            method_name=method_name,
            doc=doc,
            status=status,
            duration_s=duration,
            error_message=error_msg,
        )


def discover_tier_tests(tier: int, feature_filter: Optional[str] = None) -> unittest.TestSuite:
    """Load and optionally filter test cases for a specific tier."""
    info = TIER_MODULES[tier]
    loader = unittest.defaultTestLoader
    module = __import__(info["module"], fromlist=["*"])
    suite = loader.loadTestsFromModule(module)

    if not feature_filter:
        return suite

    # Filter suite by feature pattern (e.g. F01, F-01, 1)
    norm_filter = feature_filter.upper().replace("-", "").replace("_", "")
    if norm_filter.startswith("F") and norm_filter[1:].isdigit():
        target_f = f"F{int(norm_filter[1:]):02d}"
    elif norm_filter.isdigit():
        target_f = f"F{int(norm_filter):02d}"
    else:
        target_f = norm_filter

    filtered = unittest.TestSuite()
    for test in iter_test_cases(suite):
        name = test.id().upper()
        doc = (test.shortDescription() or "").upper()
        if target_f in name or target_f in doc:
            filtered.addTest(test)
    return filtered


def iter_test_cases(suite):
    """Recursively yield individual TestCase instances from a TestSuite."""
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from iter_test_cases(item)
        else:
            yield item


def print_banner(engine_path: Path):
    """Print master runner visual banner."""
    print("=" * 86)
    print("               DeltaUno Dual-Track E2E Test Suite Orchestrator")
    print("                     Institutional Quality Verification")
    print("=" * 86)
    print(f"Timestamp        : {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Engine Binary    : {engine_path}")
    print(f"Historical Data  : {DATA_30DAY}")
    print(f"Target Coverage  : 16 Features | 4 Tiers | 184 Total Tests")
    print("=" * 86)
    print()


def print_tier_summary_table(results_by_tier: Dict[int, DetailedTestResult], total_duration: float):
    """Print the high-level 4-tier summary table."""
    print("+" + "-" * 8 + "+" + "-" * 34 + "+" + "-" * 8 + "+" + "-" * 8 + "+" + "-" * 8 + "+" + "-" * 10 + "+" + "-" * 9 + "+")
    print(f"| {'Tier':<6} | {'Tier Description':<32} | {'Total':<6} | {'Pass':<6} | {'Fail':<6} | {'Time (s)':<8} | {'Status':<7} |")
    print("+" + "=" * 8 + "+" + "=" * 34 + "+" + "=" * 8 + "+" + "=" * 8 + "+" + "=" * 8 + "+" + "=" * 10 + "+" + "=" * 9 + "+")

    grand_total = 0
    grand_pass = 0
    grand_fail = 0

    for tier in sorted(results_by_tier.keys()):
        res = results_by_tier[tier]
        info = TIER_MODULES[tier]
        total = len(res.records)
        passed = sum(1 for r in res.records if r.status == "PASS")
        failed = sum(1 for r in res.records if r.status in ("FAIL", "ERROR"))
        tier_time = sum(r.duration_s for r in res.records)
        status = "PASSED" if failed == 0 and total > 0 else ("FAILED" if failed > 0 else "EMPTY")

        grand_total += total
        grand_pass += passed
        grand_fail += failed

        status_colored = f"\033[92m{status:<7}\033[0m" if status == "PASSED" else f"\033[91m{status:<7}\033[0m"
        print(f"| Tier {tier} | {info['name']:<32} | {total:<6} | {passed:<6} | {failed:<6} | {tier_time:<8.2f} | {status_colored} |")

    print("+" + "-" * 8 + "+" + "-" * 34 + "+" + "-" * 8 + "+" + "-" * 8 + "+" + "-" * 8 + "+" + "-" * 10 + "+" + "-" * 9 + "+")
    overall_status = "PASSED" if grand_fail == 0 and grand_total > 0 else "FAILED"
    overall_colored = f"\033[92m{overall_status:<7}\033[0m" if overall_status == "PASSED" else f"\033[91m{overall_status:<7}\033[0m"
    print(f"| {'TOTAL':<6} | {'All 4 Verification Tiers':<32} | {grand_total:<6} | {grand_pass:<6} | {grand_fail:<6} | {total_duration:<8.2f} | {overall_colored} |")
    print("+" + "-" * 8 + "+" + "-" * 34 + "+" + "-" * 8 + "+" + "-" * 8 + "+" + "-" * 8 + "+" + "-" * 10 + "+" + "-" * 9 + "+")
    print()


def print_feature_matrix(records: List[TestRecord]):
    """Print per-feature coverage breakdown across Tiers 1 and 2."""
    print("Feature Coverage Matrix (Tiers 1 & 2):")
    print("+" + "-" * 6 + "+" + "-" * 42 + "+" + "-" * 10 + "+" + "-" * 10 + "+" + "-" * 10 + "+")
    print(f"| {'ID':<4} | {'Feature Name':<40} | {'Tier 1':<8} | {'Tier 2':<8} | {'Total':<8} |")
    print("+" + "=" * 6 + "+" + "=" * 42 + "+" + "=" * 10 + "+" + "=" * 10 + "+" + "=" * 10 + "+")

    for f_id, f_name in sorted(FEATURE_NAMES.items()):
        # Count passes in Tier 1 and Tier 2 matching f_id
        f_key = f_id.lower()
        t1_count = sum(1 for r in records if r.tier == 1 and (f_key in r.method_name.lower() or f_key in r.class_name.lower()))
        t2_count = sum(1 for r in records if r.tier == 2 and (f_key in r.method_name.lower() or f_key in r.class_name.lower() or f"b{int(f_id[1:]):02d}" in r.method_name.lower()))
        total_f = t1_count + t2_count
        print(f"| {f_id:<4} | {f_name:<40} | {t1_count:<8} | {t2_count:<8} | {total_f:<8} |")

    print("+" + "-" * 6 + "+" + "-" * 42 + "+" + "-" * 10 + "+" + "-" * 10 + "+" + "-" * 10 + "+")
    print()


def main():
    parser = argparse.ArgumentParser(description="DeltaUno Master E2E Test Suite Runner")
    parser.add_argument(
        "--tier",
        type=str,
        default="all",
        help="Tier(s) to execute: 1, 2, 3, 4, 'all', or comma-separated list e.g. '1,4' (default: all)",
    )
    parser.add_argument(
        "--feature",
        type=str,
        default=None,
        help="Filter tests by feature identifier (e.g. F01, F09, 16)",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List all matching tests without running them",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Verbose output displaying each individual test name and status",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Quiet output showing only summary tables and grand total",
    )

    args = parser.parse_args()

    # Determine tiers to run
    if args.tier.lower() == "all":
        selected_tiers = [1, 2, 3, 4]
    else:
        try:
            selected_tiers = [int(t.strip()) for t in args.tier.split(",") if t.strip()]
        except ValueError:
            print(f"Error: Invalid --tier argument '{args.tier}'. Expected numbers 1-4 or 'all'.")
            sys.exit(2)

    invalid_tiers = [t for t in selected_tiers if t not in TIER_MODULES]
    if invalid_tiers or not selected_tiers:
        print(f"Error: Invalid --tier argument '{args.tier}'. Expected numbers 1-4 or 'all'.")
        sys.exit(2)

    # Pre-flight cleanup of any stale artifacts in REPO_ROOT
    for stale_name in ("deltauno.log", "deltauno_state.json"):
        stale_path = REPO_ROOT / stale_name
        if stale_path.exists():
            try:
                stale_path.unlink()
            except OSError:
                pass

    # Verify engine binary
    try:
        engine_bin = find_engine_executable()
    except FileNotFoundError as e:
        print(f"Error: {e}")
        print("Please build the engine before running tests: cmake --build build --config Release")
        sys.exit(2)

    # Handle --list
    if args.list:
        print(f"Listing E2E Tests (Tiers: {selected_tiers}, Feature filter: {args.feature or 'None'}):")
        total_found = 0
        for tier in selected_tiers:
            suite = discover_tier_tests(tier, args.feature)
            tests = list(iter_test_cases(suite))
            total_found += len(tests)
            print(f"\n--- {TIER_MODULES[tier]['name']} ({len(tests)} tests) ---")
            for t in tests:
                doc = (t.shortDescription() or "").strip()
                print(f"  {t.id()} : {sanitize_output(doc)}")
        print(f"\nTotal Matching Tests: {total_found}")
        if total_found == 0:
            if args.feature:
                print(f"Error: No tests found matching feature '{args.feature}'. Valid features are F01 through F16.")
            else:
                print("Error: No tests found matching specified criteria.")
            sys.exit(2)
        sys.exit(0)

    # Run tests
    if not args.quiet:
        print_banner(engine_bin)

    results_by_tier: Dict[int, DetailedTestResult] = {}
    all_records: List[TestRecord] = []
    overall_start = time.perf_counter()

    for tier in selected_tiers:
        info = TIER_MODULES[tier]
        if not args.quiet:
            print(f"Executing {info['name']} ...")
        suite = discover_tier_tests(tier, args.feature)
        result = DetailedTestResult(tier=tier, verbose=args.verbose)
        suite.run(result)
        results_by_tier[tier] = result
        all_records.extend(result.records)
        if not args.quiet and not args.verbose:
            pass_cnt = sum(1 for r in result.records if r.status == "PASS")
            fail_cnt = sum(1 for r in result.records if r.status in ("FAIL", "ERROR"))
            print(f"  Finished {info['name']}: {pass_cnt}/{len(result.records)} passed ({sum(r.duration_s for r in result.records):.2f}s)")
            print()

    total_duration = time.perf_counter() - overall_start

    # Print summaries
    print_tier_summary_table(results_by_tier, total_duration)

    if not args.feature:
        print_feature_matrix(all_records)

    # Print failures if any
    failed_records = [r for r in all_records if r.status in ("FAIL", "ERROR")]
    if failed_records:
        print("=" * 86)
        print(f"FAILURES AND ERRORS ({len(failed_records)} encountered):")
        print("=" * 86)
        for r in failed_records:
            print(f"\n[{r.status}] {r.test_id}")
            print(f"Doc: {sanitize_output(r.doc)}")
            print("-" * 60)
            print(r.error_message.strip())
            print("-" * 60)
        print()

    # Grand Verdict Banner
    total_tests = len(all_records)
    total_passed = sum(1 for r in all_records if r.status == "PASS")
    pass_pct = (total_passed / total_tests * 100.0) if total_tests > 0 else 0.0

    print("=" * 86)
    if total_tests == 0:
        if args.feature:
            print(f"  ERROR: No tests found matching feature filter '{args.feature}'. Valid features are F01 through F16.")
        else:
            print("  ERROR: No tests were executed.")
        print("=" * 86)
        sys.exit(2)
    elif failed_records:
        print(f"  VERDICT: \033[91mFAILED\033[0m -- {len(failed_records)}/{total_tests} tests failed.")
        print("=" * 86)
        sys.exit(1)
    else:
        print(f"  VERDICT: \033[92mPASSED\033[0m -- {total_passed}/{total_tests} tests passed (100.0%) in {total_duration:.2f}s.")
        print("=" * 86)
        sys.exit(0)


if __name__ == "__main__":
    main()
