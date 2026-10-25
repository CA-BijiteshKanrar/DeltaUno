#!/usr/bin/env python3
"""
DeltaUno 30-Day Multi-Symbol Market Data Pipeline (Milestone 1)
Generates high-fidelity, tick-aligned Level 2 market data for 5 NSE equities:
  - TATASTEEL (349121)
  - SBIN (779521)
  - ITC (424961)
  - COALINDIA (521729)
  - ZOMATO (895745, mapped to ETERNAL.NS on Yahoo Finance)

Calendar Span: 30 trading days (2026-11-02 to 2026-12-11)
Trading Hours: 09:15:00 to 15:30:00 IST (UTC+05:30)

Key Invariants:
  1. Exact 12-column CSV schema conforming to CSVFeed.
  2. All price fields (bid0_p, ask0_p, last_price_inr, upper_c, lower_c) rounded strictly to INR 0.05.
  3. Positive spread (ask0_p > bid0_p) by at least INR 0.05 on 100% of rows.
  4. Monotonic volume accumulation per symbol within each session.
  5. Strictly non-decreasing, monotonic epoch nanosecond timestamps (t_0 < t_1 < t_2).
  6. Inter-tick pacing <= 3.0s during trading hours (Watchdog safety).
  7. Cont & Kolm OFI queue dynamics calibrated for composite_ofi >= 0.55 & volume surge >= 2.5 on breakouts.
  8. Full 30-day market regime schedule matrix with institutional safeguards activation.
"""

import argparse
import collections
import datetime
import heapq
import json
import math
import os
import random
import sys
import time
from typing import Dict, List, Tuple, Any, Optional

try:
    import requests
except ImportError:
    requests = None


# --- 1. Instrument Definitions ---
INSTRUMENTS = {
    "TATASTEEL": {
        "token": 349121,
        "yahoo": "TATASTEEL.NS",
        "ref_price": 150.0,
        "upper_circuit": 165.0,
        "lower_circuit": 135.0,
        "base_qty": 200,
        "base_bid_q": 600,
        "base_ask_q": 600,
        "opening_volume": 500000,
    },
    "SBIN": {
        "token": 779521,
        "yahoo": "SBIN.NS",
        "ref_price": 800.0,
        "upper_circuit": 880.0,
        "lower_circuit": 720.0,
        "base_qty": 100,
        "base_bid_q": 500,
        "base_ask_q": 500,
        "opening_volume": 200000,
    },
    "ITC": {
        "token": 424961,
        "yahoo": "ITC.NS",
        "ref_price": 480.0,
        "upper_circuit": 528.0,
        "lower_circuit": 432.0,
        "base_qty": 150,
        "base_bid_q": 550,
        "base_ask_q": 550,
        "opening_volume": 150000,
    },
    "COALINDIA": {
        "token": 521729,
        "yahoo": "COALINDIA.NS",
        "ref_price": 460.0,
        "upper_circuit": 506.0,
        "lower_circuit": 414.0,
        "base_qty": 120,
        "base_bid_q": 500,
        "base_ask_q": 500,
        "opening_volume": 120000,
    },
    "ZOMATO": {
        "token": 895745,
        "yahoo": "ETERNAL.NS",
        "ref_price": 250.0,
        "upper_circuit": 275.0,
        "lower_circuit": 225.0,
        "base_qty": 250,
        "base_bid_q": 700,
        "base_ask_q": 700,
        "opening_volume": 350000,
    },
}

SYMBOL_LIST = ["TATASTEEL", "SBIN", "ITC", "COALINDIA", "ZOMATO"]

# 30 Weekdays: 2026-11-02 to 2026-12-11
TARGET_DATES = [
    "2026-11-02", "2026-11-03", "2026-11-04", "2026-11-05", "2026-11-06",
    "2026-11-09", "2026-11-10", "2026-11-11", "2026-11-12", "2026-11-13",
    "2026-11-16", "2026-11-17", "2026-11-18", "2026-11-19", "2026-11-20",
    "2026-11-23", "2026-11-24", "2026-11-25", "2026-11-26", "2026-11-27",
    "2026-11-30", "2026-12-01", "2026-12-02", "2026-12-03", "2026-12-04",
    "2026-12-07", "2026-12-08", "2026-12-09", "2026-12-10", "2026-12-11",
]


def round_nse_tick(price: float) -> float:
    """Rounds price to the nearest INR 0.05 tick size."""
    return round(round(price / 0.05) * 0.05, 2)


def ist_datetime_to_utc_ns(ymd: str, hms: str, subsecond_ns: int = 0) -> int:
    """Converts IST date YYYY-MM-DD and time HH:MM:SS to UTC epoch nanoseconds."""
    year = int(ymd[0:4])
    month = int(ymd[5:7])
    day = int(ymd[8:10])
    parts = hms.split(":")
    hour = int(parts[0])
    minute = int(parts[1])
    second = int(parts[2]) if len(parts) > 2 else 0

    midnight_utc_sec = int(
        datetime.datetime(year, month, day, 0, 0, 0, tzinfo=datetime.timezone.utc).timestamp()
    )
    ist_sec = hour * 3600 + minute * 60 + second
    # IST offset is +05:30 = +19800 seconds
    utc_sec = midnight_utc_sec + ist_sec - 19800
    return utc_sec * 1_000_000_000 + subsecond_ns


# --- 2. Data Ingestion & Caching ---
def fetch_and_cache_raw_data(
    symbol: str, ticker: str, cache_dir: str, force_offline: bool = False
) -> Dict[str, Any]:
    """Fetches real historical 1m & 2m intraday data from Yahoo Finance and caches it."""
    os.makedirs(cache_dir, exist_ok=True)
    cache_file = os.path.join(cache_dir, f"{symbol}_raw.json")

    if os.path.exists(cache_file):
        with open(cache_file, "r", encoding="utf-8") as f:
            return json.load(f)

    if force_offline or requests is None:
        return generate_synthetic_raw_data(symbol)

    headers = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}
    try:
        # Fetch 2m data spanning 60 days
        url_2m = f"https://query1.finance.yahoo.com/v8/finance/chart/{ticker}?interval=2m&range=60d"
        r2m = requests.get(url_2m, headers=headers, timeout=15).json()

        # Fetch 1m data in 7-day chunks (last 28 days)
        now = int(time.time())
        chunks_1m = []
        for c in range(4):
            p2 = now - c * 7 * 86400
            p1 = p2 - 7 * 86400
            url_1m = f"https://query1.finance.yahoo.com/v8/finance/chart/{ticker}?interval=1m&period1={p1}&period2={p2}"
            r1m = requests.get(url_1m, headers=headers, timeout=15)
            if r1m.status_code == 200:
                chunks_1m.append(r1m.json())
            time.sleep(0.2)

        raw_data = {"r2m": r2m, "chunks_1m": chunks_1m}
        with open(cache_file, "w", encoding="utf-8") as f:
            json.dump(raw_data, f)
        return raw_data
    except Exception as e:
        print(f"Warning: Failed to fetch {ticker} from Yahoo Finance ({e}). Falling back to calibrated synthetic generator.")
        raw_data = generate_synthetic_raw_data(symbol)
        with open(cache_file, "w", encoding="utf-8") as f:
            json.dump(raw_data, f)
        return raw_data


def generate_synthetic_raw_data(symbol: str) -> Dict[str, Any]:
    """Calibrated deterministic synthetic fallback generator (100% offline)."""
    cfg = INSTRUMENTS[symbol]
    rng = random.Random(cfg["token"])
    base_price = cfg["ref_price"]

    # Generate 30 trading days of 2m bars
    timestamps = []
    opens, highs, lows, closes, volumes = [], [], [], [], []

    # 30 simulated trading days
    cur_p = base_price
    for d in range(30):
        # 187 two-minute bars per day
        day_open = cur_p * rng.uniform(0.995, 1.005)
        bar_p = day_open
        for b in range(188):
            t = 1788925500 + d * 86400 + b * 120
            delta = rng.gauss(0, base_price * 0.001)
            o = bar_p
            c = round_nse_tick(max(cfg["lower_circuit"] + 0.5, min(cfg["upper_circuit"] - 0.5, o + delta)))
            h = round_nse_tick(max(o, c) + abs(rng.gauss(0, base_price * 0.0005)))
            l = round_nse_tick(min(o, c) - abs(rng.gauss(0, base_price * 0.0005)))
            v = rng.randint(2000, 15000)
            bar_p = c

            timestamps.append(t)
            opens.append(o)
            highs.append(h)
            lows.append(l)
            closes.append(c)
            volumes.append(v)
        cur_p = bar_p

    r2m = {
        "chart": {
            "result": [{
                "meta": {"symbol": cfg["yahoo"]},
                "timestamp": timestamps,
                "indicators": {"quote": [{
                    "open": opens, "high": highs, "low": lows, "close": closes, "volume": volumes
                }]}
            }]
        }
    }
    return {"r2m": r2m, "chunks_1m": []}


def process_symbol_bars(symbol: str, raw_data: Dict[str, Any]) -> List[List[Dict[str, float]]]:
    """Parses raw Yahoo JSON data into exactly 30 trading days of 1-minute bars."""
    cfg = INSTRUMENTS[symbol]
    ref_price = cfg["ref_price"]

    # 1. Collect all high-resolution 1m bars from chunks
    bars_1m = {}
    for chunk in raw_data.get("chunks_1m", []):
        try:
            res = chunk["chart"]["result"][0]
            ts_list = res.get("timestamp", [])
            quotes = res["indicators"]["quote"][0]
            opens = quotes.get("open", [])
            highs = quotes.get("high", [])
            lows = quotes.get("low", [])
            closes = quotes.get("close", [])
            vols = quotes.get("volume", [])
            for i, t in enumerate(ts_list):
                if i < len(closes) and closes[i] is not None:
                    bars_1m[t] = {
                        "open": float(opens[i]) if opens[i] is not None else float(closes[i]),
                        "high": float(highs[i]) if highs[i] is not None else float(closes[i]),
                        "low": float(lows[i]) if lows[i] is not None else float(closes[i]),
                        "close": float(closes[i]),
                        "volume": int(vols[i]) if vols[i] is not None else 0,
                    }
        except (KeyError, IndexError):
            continue

    # 2. Extract 2m data and group by trading day
    res2m = raw_data["r2m"]["chart"]["result"][0]
    ts2m = res2m.get("timestamp", [])
    q2m = res2m["indicators"]["quote"][0]
    opens2 = q2m.get("open", [])
    highs2 = q2m.get("high", [])
    lows2 = q2m.get("low", [])
    closes2 = q2m.get("close", [])
    vols2 = q2m.get("volume", [])

    days_map: Dict[str, List[Dict[str, Any]]] = {}
    prev_close = ref_price

    for i, t in enumerate(ts2m):
        dt = datetime.datetime.fromtimestamp(t, datetime.timezone(datetime.timedelta(hours=5, minutes=30)))
        d_str = dt.strftime("%Y-%m-%d")
        if d_str not in days_map:
            days_map[d_str] = []

        # Handle null bars with forward-fill
        c = closes2[i] if (i < len(closes2) and closes2[i] is not None) else prev_close
        o = opens2[i] if (i < len(opens2) and opens2[i] is not None) else c
        h = highs2[i] if (i < len(highs2) and highs2[i] is not None) else max(o, c)
        l = lows2[i] if (i < len(lows2) and lows2[i] is not None) else min(o, c)
        v = vols2[i] if (i < len(vols2) and vols2[i] is not None) else 0
        prev_close = c

        days_map[d_str].append({"ts": t, "open": o, "high": h, "low": l, "close": c, "volume": v})

    # Sort days chronologically and keep the last 30
    sorted_days = sorted(days_map.keys())
    if len(sorted_days) > 30:
        sorted_days = sorted_days[-30:]

    processed_days = []
    for d_str in sorted_days:
        day_bars_2m = days_map[d_str]
        # Reconstruct into 375 one-minute bars
        day_1m = []
        for b in day_bars_2m:
            t = b["ts"]
            # If 1m data exists for this timestamp, prefer it
            if t in bars_1m and (t + 60) in bars_1m:
                day_1m.append(bars_1m[t])
                day_1m.append(bars_1m[t + 60])
            else:
                # Interpolate 2m bar into two 1m bars
                o, h, l, c, v = b["open"], b["high"], b["low"], b["close"], b["volume"]
                mid = (o + c) / 2.0
                bar1 = {
                    "open": o,
                    "high": max(o, mid, (h + max(o, c)) / 2.0),
                    "low": min(o, mid, (l + min(o, c)) / 2.0),
                    "close": mid,
                    "volume": v // 2,
                }
                bar2 = {
                    "open": mid,
                    "high": h,
                    "low": l,
                    "close": c,
                    "volume": v - v // 2,
                }
                day_1m.append(bar1)
                day_1m.append(bar2)

        # Pad or trim to exactly 375 minutes (09:15 to 15:30 IST)
        if len(day_1m) < 375:
            last_b = day_1m[-1] if day_1m else {"open": ref_price, "high": ref_price, "low": ref_price, "close": ref_price, "volume": 0}
            while len(day_1m) < 375:
                day_1m.append({"open": last_b["close"], "high": last_b["close"], "low": last_b["close"], "close": last_b["close"], "volume": 0})
        elif len(day_1m) > 375:
            day_1m = day_1m[:375]

        # Normalize price series around instrument's reference price to maintain exact alignment with risk rules
        base_open = day_1m[0]["open"]
        if base_open <= 0: base_open = ref_price
        scale_ratio = ref_price / base_open

        scaled_day_bars = []
        for bar in day_1m:
            scaled_day_bars.append({
                "open": round_nse_tick(bar["open"] * scale_ratio),
                "high": round_nse_tick(bar["high"] * scale_ratio),
                "low": round_nse_tick(bar["low"] * scale_ratio),
                "close": round_nse_tick(bar["close"] * scale_ratio),
                "volume": bar["volume"],
            })
        processed_days.append(scaled_day_bars)

    # If fewer than 30 days available, pad with copies
    while len(processed_days) < 30:
        processed_days.append(processed_days[-1] if processed_days else [])

    return processed_days[:30]


# --- 3. Microstructure & Regime Simulation ---
def generate_symbol_session_ticks(
    symbol: str,
    day_idx: int,       # 1 to 30
    cal_date: str,      # YYYY-MM-DD
    day_bars: List[Dict[str, float]],
    seed: int = 42,
) -> List[Tuple[int, str]]:
    """
    Generates chronological Level-2 ticks for a single symbol on a single trading day.
    Returns list of (timestamp_ns, formatted_csv_line).
    """
    cfg = INSTRUMENTS[symbol]
    token = cfg["token"]
    ref_price = cfg["ref_price"]
    upper_c = cfg["upper_circuit"]
    lower_c = cfg["lower_circuit"]
    base_qty = cfg["base_qty"]
    base_bid_q = cfg["base_bid_q"]
    base_ask_q = cfg["base_ask_q"]

    sym_idx = SYMBOL_LIST.index(symbol)
    rng = random.Random(seed + day_idx * 100 + token)

    session_ticks: List[Tuple[int, str]] = []
    cum_volume = cfg["opening_volume"] + rng.randint(10000, 50000)

    # Session turnover & volume for running session VWAP calculation
    session_turnover = 0.0
    session_vol = 0

    # Base cadence: each symbol emits a tick every 6 seconds, staggered across the 5 symbols
    # Interval between consecutive ticks across symbols = 6 / 5 = 1.2s <= 3.0s strictly!
    base_sec_offset = round(sym_idx * 1.2, 2)

    # Current market price state
    current_price = day_bars[0]["open"] if day_bars else ref_price
    current_price = round_nse_tick(current_price)

    # Daily special regime parameters
    is_breakout_day = False
    breakout_start_sec = 0
    breakout_end_sec = 0
    tp_target_price = 0.0

    is_circuit_test_day = False
    is_breakeven_test_day = False
    is_kill_switch_test_day = False
    is_auto_sq_day = False
    is_watchdog_pause_day = False
    is_chop_day = (day_idx in [2, 11, 19])

    # Check Day Regime Matrix
    if day_idx == 1 and symbol == "TATASTEEL":
        is_breakout_day = True
        breakout_start_sec = 34200 + 15 * 60  # 09:45:00 IST (seconds from midnight: 35100)
        breakout_end_sec = breakout_start_sec + 20 * 60
        tp_target_price = round_nse_tick(current_price * 1.018)
    elif day_idx == 3 and symbol == "ITC":
        is_breakeven_test_day = True
        breakout_start_sec = 34200 + 5 * 60   # 09:35:00 IST
        breakout_end_sec = breakout_start_sec + 25 * 60
    elif day_idx == 4 and symbol == "SBIN":
        is_breakout_day = True
        breakout_start_sec = 48600 + 15 * 60  # 13:45:00 IST
        breakout_end_sec = breakout_start_sec + 25 * 60
        tp_target_price = round_nse_tick(current_price * 1.018)
    elif day_idx == 5 and symbol == "COALINDIA":
        # Afternoon breakout after FOMC freeze
        is_breakout_day = True
        breakout_start_sec = 48600 + 10 * 60  # 13:40:00 IST
        breakout_end_sec = breakout_start_sec + 25 * 60
        tp_target_price = round_nse_tick(current_price * 1.018)
    elif day_idx == 6 and symbol == "ZOMATO":
        # +3.5% overnight gap open
        current_price = round_nse_tick(ref_price * 1.035)
    elif day_idx == 8 and symbol == "COALINDIA":
        is_circuit_test_day = True
        breakout_start_sec = 34200 + 30 * 60  # 10:00:00 IST
    elif day_idx == 9 and (symbol == "TATASTEEL" or symbol == "SBIN"):
        is_breakout_day = True
        breakout_start_sec = 34200 + 10 * 60  # 09:40:00 IST
        breakout_end_sec = breakout_start_sec + 15 * 60
        tp_target_price = round_nse_tick(current_price * 1.018)
    elif day_idx == 10 and symbol == "ITC":
        # Adverse stop-loss hit
        is_breakout_day = True
        breakout_start_sec = 34200 + 15 * 60  # 09:45:00 IST
        breakout_end_sec = breakout_start_sec + 15 * 60
        tp_target_price = -1.0  # signals adverse drop
    elif day_idx == 12 and symbol == "ZOMATO":
        is_breakout_day = True
        breakout_start_sec = 34200 + 45 * 60  # 10:15:00 IST
        breakout_end_sec = breakout_start_sec + 20 * 60
        tp_target_price = round_nse_tick(current_price * 1.018)
    elif day_idx == 15 and symbol == "SBIN":
        is_auto_sq_day = True
        breakout_start_sec = 48600 + 40 * 60  # 14:10:00 IST
        breakout_end_sec = 54300 + 5 * 60     # past 15:05:00
    elif day_idx == 16 and symbol == "COALINDIA":
        is_breakout_day = True
        breakout_start_sec = 34200 + 20 * 60  # 09:50:00 IST
        breakout_end_sec = breakout_start_sec + 20 * 60
        tp_target_price = round_nse_tick(current_price * 1.018)
    elif day_idx == 17 and (symbol == "TATASTEEL" or symbol == "SBIN"):
        is_kill_switch_test_day = True
        breakout_start_sec = 34200 + 5 * 60   # 09:35:00 IST
    elif day_idx == 18 and symbol == "TATASTEEL":
        is_breakout_day = True
        breakout_start_sec = 34200 + 30 * 60  # 10:00:00 IST
        breakout_end_sec = breakout_start_sec + 20 * 60
        tp_target_price = round_nse_tick(current_price * 1.018)
    elif day_idx == 20 and symbol == "SBIN":
        is_breakout_day = True
        breakout_start_sec = 48600 + 5 * 60   # 13:35:00 IST
        breakout_end_sec = breakout_start_sec + 25 * 60
        tp_target_price = round_nse_tick(current_price * 1.018)
    elif day_idx == 21 and symbol == "COALINDIA":
        # -2.5% gap down at open, afternoon breakout
        current_price = round_nse_tick(ref_price * 0.975)
        is_breakout_day = True
        breakout_start_sec = 48600 + 10 * 60  # 13:40:00 IST
        breakout_end_sec = breakout_start_sec + 20 * 60
        tp_target_price = round_nse_tick(current_price * 1.018)
    elif day_idx == 22:
        is_watchdog_pause_day = True
    elif day_idx == 26 and symbol == "ITC":
        is_breakout_day = True
        breakout_start_sec = 34200 + 25 * 60  # 09:55:00 IST
        breakout_end_sec = breakout_start_sec + 20 * 60
        tp_target_price = round_nse_tick(current_price * 1.018)
    elif day_idx == 27 and symbol == "ZOMATO":
        is_circuit_test_day = True
        breakout_start_sec = 34200 + 45 * 60  # 10:00:00 IST
    elif day_idx == 28 and symbol == "COALINDIA":
        is_breakeven_test_day = True
        breakout_start_sec = 34200 + 10 * 60  # 09:40:00 IST
        breakout_end_sec = breakout_start_sec + 25 * 60
    elif day_idx == 29 and symbol == "TATASTEEL":
        is_breakout_day = True
        breakout_start_sec = 48600 + 30 * 60  # 14:00:00 IST
        breakout_end_sec = breakout_start_sec + 20 * 60
        tp_target_price = round_nse_tick(current_price * 1.018)

    # 375 minutes in trading session (09:15 to 15:30 IST)
    # Total seconds = 22,500s. Seconds from midnight: 33,300 to 55,800.
    start_session_sec = 33300
    end_session_sec = 55800

    has_watchdog_paused = False
    sec = start_session_sec + int(base_sec_offset)
    while sec < end_session_sec:
        # Watchdog verification pause on Day 22 at 10:30 IST (37,800s): pause for 6s once while preserving stagger
        if is_watchdog_pause_day and not has_watchdog_paused and sec >= 37800:
            sec += 6
            has_watchdog_paused = True

        minute_idx = min(374, (sec - start_session_sec) // 60)
        bar = day_bars[minute_idx] if minute_idx < len(day_bars) else day_bars[-1]

        # Determine tick dynamics based on active regime
        in_breakout = (is_breakout_day and breakout_start_sec <= sec <= breakout_end_sec)
        in_pre_breakout = (is_breakout_day and (breakout_start_sec - 900) <= sec < breakout_start_sec)
        in_circuit_test = (is_circuit_test_day and breakout_start_sec <= sec <= breakout_start_sec + 180)
        in_be_test = (is_breakeven_test_day and breakout_start_sec <= sec <= breakout_end_sec)
        in_ks_test = (is_kill_switch_test_day and breakout_start_sec <= sec <= breakout_start_sec + 600)
        in_auto_sq = (is_auto_sq_day and breakout_start_sec <= sec <= breakout_end_sec)

        cur_vwap = (session_turnover / session_vol) if session_vol > 0 else current_price
        spread_ticks = 1

        if in_circuit_test:
            # Push price within 1.3% of upper or lower circuit
            if day_idx == 8:  # COALINDIA Upper Circuit Test (₹506.00)
                current_price = round_nse_tick(upper_c * 0.987)  # ₹499.40 (within 1.3% of upper)
                delta = 0.05
                qty = rng.randint(2500, 4000)
                bid_q = rng.randint(5000, 8000)
                ask_q = rng.randint(100, 250)
            else:             # ZOMATO Lower Circuit Test (₹225.00)
                current_price = round_nse_tick(lower_c * 1.013)  # ₹228.00 (within 1.33% of lower)
                delta = -0.05
                qty = rng.randint(2500, 4000)
                bid_q = rng.randint(100, 250)
                ask_q = rng.randint(5000, 8000)
        elif in_breakout:
            # High-volume momentum breakout setup & follow-through
            rel_sec = sec - breakout_start_sec
            if rel_sec < 40:
                # Pre-condition price above VWAP before the volume surge fires
                target_p = round_nse_tick(cur_vwap + 0.20)
                if current_price < target_p:
                    delta = 0.05
                elif current_price > target_p + 0.10:
                    delta = -0.05
                else:
                    delta = rng.choice([-0.05, 0.0, 0.05])
                qty = rng.randint(base_qty, base_qty + 50)
                bid_q = rng.randint(base_bid_q - 50, base_bid_q + 50)
                ask_q = rng.randint(base_ask_q - 50, base_ask_q + 50)
            elif rel_sec < 180:
                # Massive institutional buyer expansion (composite_ofi >= 0.55 & vol >= 2.5x)
                delta = 0.05
                qty = rng.randint(base_qty * 8, base_qty * 14)  # 2,000 to 3,500 shares
                bid_q = rng.randint(3500, 5200)                 # Deep bid support
                ask_q = rng.randint(150, 300)                   # Depleted ask
            else:
                # Follow through to Take-Profit or Stop-Loss
                if tp_target_price > 0 and current_price < tp_target_price:
                    delta = rng.choice([0.05, 0.10, 0.0, 0.05])
                elif tp_target_price < 0:
                    delta = -0.10  # Adverse drop to hit Stop-Loss
                else:
                    delta = rng.choice([-0.05, 0.0, 0.05])
                qty = rng.randint(base_qty * 2, base_qty * 4)
                bid_q = rng.randint(1200, 2200)
                ask_q = rng.randint(600, 1100)
        elif in_be_test:
            rel_sec = sec - breakout_start_sec
            if rel_sec < 40:
                delta = 0.0
                qty = base_qty
                bid_q = base_bid_q
                ask_q = base_ask_q
            elif rel_sec < 180:
                # Rally +1.1% to activate Breakeven Stop Ratchet
                delta = 0.10
                qty = rng.randint(base_qty * 5, base_qty * 10)
                bid_q = rng.randint(3000, 4500)
                ask_q = rng.randint(200, 400)
            else:
                # Sudden reversal dropping price back to breakeven stop
                delta = -0.15
                qty = rng.randint(base_qty * 2, base_qty * 4)
                bid_q = rng.randint(300, 600)
                ask_q = rng.randint(2500, 4000)
        elif in_ks_test:
            # Adverse execution triggering ₹500 daily drawdown kill-switch
            delta = -0.15
            qty = rng.randint(base_qty * 4, base_qty * 8)
            bid_q = rng.randint(200, 400)
            ask_q = rng.randint(3500, 5000)
        elif in_auto_sq:
            # Afternoon entry held until 15:05:00 IST forced exit
            if sec < 54300:
                delta = rng.choice([-0.05, 0.05, 0.0])
                qty = base_qty
                bid_q = base_bid_q
                ask_q = base_ask_q
            else:
                delta = -0.05
                qty = base_qty * 3
                bid_q = base_bid_q
                ask_q = base_ask_q
        elif in_pre_breakout:
            # 15-minute pre-breakout accumulation ramp: steer current_price smoothly towards cur_vwap + 0.20
            target_p = round_nse_tick(cur_vwap + 0.20)
            if current_price < target_p:
                delta = 0.05
            elif current_price > target_p + 0.10:
                delta = -0.05
            else:
                delta = rng.choice([-0.05, 0.0, 0.05])
            qty = max(20, min(1000, int(bar["volume"] // 60) + rng.randint(-30, 50)))
            bid_q = rng.randint(base_bid_q - 50, base_bid_q + 50)
            ask_q = rng.randint(base_ask_q - 50, base_ask_q + 50)
        elif is_chop_day:
            # Rangebound quiet chop regime (capital preservation benchmark: 0 trades, OFI in [-0.15, +0.15], vol_mult < 1.3x)
            spread_ticks = 2
            delta = 0.0
            qty = rng.randint(base_qty - 20, base_qty + 20)
            bid_q = base_bid_q + rng.randint(-10, 10)
            ask_q = base_ask_q + rng.randint(-10, 10)
        else:
            # Normal realistic trading matching real bar prices
            bar_target = bar["close"]
            if current_price < bar_target:
                delta = 0.05
            elif current_price > bar_target:
                delta = -0.05
            else:
                delta = rng.choice([-0.05, 0.0, 0.05])
            qty = max(20, min(1000, int(bar["volume"] // 60) + rng.randint(-30, 50)))
            bid_q = rng.randint(base_bid_q - 100, base_bid_q + 100)
            ask_q = rng.randint(base_ask_q - 100, base_ask_q + 100)

        # Update price within circuit limits
        current_price = round_nse_tick(
            max(lower_c + 0.10, min(upper_c - 0.10, current_price + delta))
        )
        cum_volume += qty
        session_turnover += current_price * qty
        session_vol += qty

        # Top-of-book prices with positive spread invariant (ask0_p - bid0_p >= 0.05)
        bid0_p = round_nse_tick(current_price - 0.05 * (spread_ticks // 2))
        ask0_p = round_nse_tick(bid0_p + 0.05 * spread_ticks)

        # Microsecond subsecond jitter for unique ordering
        sub_ns = (sym_idx * 150_000_000) + rng.randint(1_000, 50_000)
        ts_ns = ist_datetime_to_utc_ns(cal_date, f"{sec // 3600:02d}:{(sec % 3600) // 60:02d}:{sec % 60:02d}", sub_ns)

        line = (
            f"{ts_ns},{token},{symbol},{current_price:.2f},{qty},{cum_volume},"
            f"{bid0_p:.2f},{bid_q},{ask0_p:.2f},{ask_q},{upper_c:.2f},{lower_c:.2f}"
        )
        session_ticks.append((ts_ns, line))

        # Advance time by 6 seconds (interleaved gap across 5 symbols = 1.2s <= 3.0s)
        step = 6
        if in_breakout or in_circuit_test or in_be_test:
            step = 2  # Tighter pacing during active micro-events
        sec += step

    return session_ticks


# --- 4. External Alert Webhooks Injection ---
def generate_embedded_alerts(cal_date: str, day_idx: int) -> List[Tuple[int, str]]:
    """Generates #ALERT, rows for testing SignalIngestor and RiskManager gates."""
    alerts = []
    if day_idx == 5:
        # Day 5: FOMC Freeze window test (09:15-10:00 IST). Alert @ 09:35 IST should be vetoed with MACRO_EVENT_FREEZE.
        ts_ns = ist_datetime_to_utc_ns(cal_date, "09:35:00")
        line = f"#ALERT,{ts_ns},521729,COALINDIA,BUY,460.00,457.00"
        alerts.append((ts_ns, line))
    elif day_idx == 7:
        # Day 7: Payoff-to-Fee Ratio gate test. Scalp signal with tight 5-paise TP vetoed with INSUFFICIENT_PAYOFF_TO_FEE_RATIO.
        ts_ns = ist_datetime_to_utc_ns(cal_date, "09:40:00")
        line = f"#ALERT,{ts_ns},349121,TATASTEEL,BUY,150.00,149.95"
        alerts.append((ts_ns, line))
    elif day_idx == 13:
        # Day 13: Whitelist Guard. SUZLON is not in whitelist; vetoed with NOT_IN_WHITELIST.
        ts_ns = ist_datetime_to_utc_ns(cal_date, "10:00:00")
        line = f"#ALERT,{ts_ns},999999,SUZLON,BUY,50.00,48.00"
        alerts.append((ts_ns, line))
    elif day_idx == 14:
        # Day 14: Overcrowding / Pump Trap. Alert injected after surge; vetoed with OVERCROWDED_EXIT_TRAP.
        ts_ns = ist_datetime_to_utc_ns(cal_date, "10:20:00")
        line = f"#ALERT,{ts_ns},349121,TATASTEEL,BUY,154.00,150.00"
        alerts.append((ts_ns, line))
    elif day_idx == 25:
        # Day 25: RBI Policy Freeze window test (09:45-11:30 IST). Alert @ 10:15 IST vetoed with MACRO_EVENT_FREEZE.
        ts_ns = ist_datetime_to_utc_ns(cal_date, "10:15:00")
        line = f"#ALERT,{ts_ns},424961,ITC,BUY,480.00,476.00"
        alerts.append((ts_ns, line))

    return alerts


# --- 5. Main Interleaved Dataset Assembly ---
def generate_30day_dataset(
    output_csv: str = "data/historical_30days_nse.csv",
    cache_dir: str = "data/cache_1m",
    force_offline: bool = False,
) -> None:
    """Generates the full 30-day multi-symbol historical L2 tick dataset."""
    print("=" * 65)
    print(" DeltaUno 30-Day Multi-Symbol Market Data Pipeline")
    print(f" Target Output: {output_csv}")
    print(f" Cache Directory: {cache_dir}")
    print(f" Calendar Range: {TARGET_DATES[0]} to {TARGET_DATES[-1]} (30 Trading Sessions)")
    print("=" * 65)

    # 1. Fetch & process bars for all 5 instruments
    all_symbol_bars: Dict[str, List[List[Dict[str, float]]]] = {}
    for sym in SYMBOL_LIST:
        print(f"[*] Processing 1-minute historical data for {sym}...")
        raw = fetch_and_cache_raw_data(sym, INSTRUMENTS[sym]["yahoo"], cache_dir, force_offline)
        processed = process_symbol_bars(sym, raw)
        all_symbol_bars[sym] = processed
        print(f"    -> Prepared {len(processed)} daily sessions ({len(processed[0])} bars/day)")

    # 2. Open output file for streaming write
    os.makedirs(os.path.dirname(os.path.abspath(output_csv)), exist_ok=True)
    total_ticks = 0
    start_time = time.time()

    with open(output_csv, "w", encoding="utf-8", newline="\n") as out_f:
        # Write authoritative 12-column header
        out_f.write("#timestamp_ns,token,symbol,last_price_inr,last_qty,volume,bid0_p,bid0_q,ask0_p,ask0_q,upper_c,lower_c\n")

        last_written_ts = 0

        # Iterate day by day (1 to 30)
        for day_idx in range(1, 31):
            cal_date = TARGET_DATES[day_idx - 1]
            day_generators = []

            # Generate ticks for all 5 symbols for this day
            for sym in SYMBOL_LIST:
                bars = all_symbol_bars[sym][day_idx - 1]
                ticks = generate_symbol_session_ticks(sym, day_idx, cal_date, bars)
                day_generators.append(ticks)

            # Include embedded external alerts for this day
            alerts = generate_embedded_alerts(cal_date, day_idx)
            if alerts:
                day_generators.append(alerts)

            # Streaming K-way merge for chronological order within the session
            merged_day = heapq.merge(*day_generators, key=lambda x: x[0])

            day_tick_count = 0
            for ts_ns, line in merged_day:
                # Handle alert lines
                if line.startswith("#ALERT,"):
                    out_f.write(line + "\n")
                    continue

                # Enforce strictly monotonic timestamps (t_0 < t_1 < t_2)
                if ts_ns <= last_written_ts:
                    ts_ns = last_written_ts + 100_000  # +100 microseconds

                last_written_ts = ts_ns

                # Reconstruct line with monotonically adjusted timestamp
                parts = line.split(",", 1)
                final_line = f"{ts_ns},{parts[1]}"
                out_f.write(final_line + "\n")

                day_tick_count += 1
                total_ticks += 1

            if day_idx % 5 == 0 or day_idx == 1:
                print(f" [+] Day {day_idx:2d} ({cal_date}): {day_tick_count:,} ticks written (Cumulative: {total_ticks:,})")

    elapsed = time.time() - start_time
    file_size_mb = os.path.getsize(output_csv) / (1024 * 1024)
    print("=" * 65)
    print(f" [SUCCESS] Generated {total_ticks:,} ticks across 30 sessions in {elapsed:.2f}s")
    print(f" Output File Size: {file_size_mb:.2f} MB -> {output_csv}")
    print("=" * 65)


# --- 6. Dataset Validation Oracle (Microstructure & Financial Invariants) ---
K_LEVEL_WEIGHTS = (1.00, 0.70, 0.50, 0.35, 0.25)


class SimOrderBook:
    """Exact Python mirror of DeltaUno C++ OrderBook (include/deltauno/market/order_book.hpp)."""
    __slots__ = ("token", "symbol", "prev_bids", "prev_asks", "has_prev", "ofi_history", "last_price", "upper_c", "lower_c")

    def __init__(self, token: int, symbol: str):
        self.token = token
        self.symbol = symbol
        self.prev_bids: Optional[Tuple[Tuple[int, int], ...]] = None
        self.prev_asks: Optional[Tuple[Tuple[int, int], ...]] = None
        self.has_prev = False
        self.ofi_history = collections.deque(maxlen=20)
        self.last_price = 0
        self.upper_c = 0
        self.lower_c = 0

    def reset(self) -> None:
        self.prev_bids = None
        self.prev_asks = None
        self.has_prev = False
        self.ofi_history.clear()
        self.last_price = 0
        self.upper_c = 0
        self.lower_c = 0

    def on_tick(self, ltp_p: int, b0_p: int, b0_q: int, a0_p: int, a0_q: int, uc_p: int, lc_p: int) -> float:
        self.last_price = ltp_p
        self.upper_c = uc_p
        self.lower_c = lc_p

        # Reconstruct 5-level depth levels identically to CSVFeed (include/deltauno/market/csv_feed.hpp)
        bids = (
            (b0_p, b0_q),
            (b0_p - 5, b0_q + 100),
            (b0_p - 10, b0_q + 200),
            (b0_p - 15, b0_q + 300),
            (b0_p - 20, b0_q + 400),
        )
        asks = (
            (a0_p, a0_q),
            (a0_p + 5, a0_q + 100),
            (a0_p + 10, a0_q + 200),
            (a0_p + 15, a0_q + 300),
            (a0_p + 20, a0_q + 400),
        )

        weighted_ofi = 0.0
        weighted_depth = 0.0

        for m in range(5):
            if self.has_prev:
                # Cont & Kolm bid queue flow
                if bids[m][0] > self.prev_bids[m][0]:
                    delta_bid = bids[m][1]
                elif bids[m][0] == self.prev_bids[m][0]:
                    delta_bid = bids[m][1] - self.prev_bids[m][1]
                else:
                    delta_bid = -self.prev_bids[m][1]

                # Cont & Kolm ask queue flow
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

    def composite_ofi(self) -> float:
        if not self.ofi_history:
            return 0.0
        ofi_1 = self.ofi_history[-1]
        c5 = min(5, len(self.ofi_history))
        ofi_5 = sum(list(self.ofi_history)[-c5:]) / float(c5)
        ofi_20 = sum(self.ofi_history) / float(len(self.ofi_history))
        return (0.5 * ofi_1) + (0.3 * ofi_5) + (0.2 * ofi_20)

    def is_near_circuit(self, buffer_pct: float = 1.5) -> bool:
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


class SimStrategy:
    """Exact Python mirror of MomentumBreakoutStrategy indicator state (include/deltauno/strategy/momentum_breakout.hpp)."""
    __slots__ = ("cum_turnover", "cum_vol", "prev_tot_vol", "recent_vols")

    def __init__(self):
        self.cum_turnover = 0.0
        self.cum_vol = 0
        self.prev_tot_vol = 0
        self.recent_vols = collections.deque(maxlen=20)

    def reset(self) -> None:
        self.cum_turnover = 0.0
        self.cum_vol = 0
        self.prev_tot_vol = 0
        self.recent_vols.clear()

    def on_tick(self, ltp_p: int, lq: int, tot_vol: int) -> None:
        if lq > 0:
            self.cum_turnover += float(ltp_p) * float(lq)
            self.cum_vol += lq

        if self.prev_tot_vol > 0 and tot_vol >= self.prev_tot_vol:
            delta_vol = tot_vol - self.prev_tot_vol
            self.recent_vols.append(delta_vol)
        self.prev_tot_vol = tot_vol

    def get_vwap(self) -> int:
        if self.cum_vol == 0:
            return 0
        return int(self.cum_turnover / float(self.cum_vol))

    def latest_volume_multiple(self) -> float:
        if len(self.recent_vols) < 2:
            return 0.0
        v_list = list(self.recent_vols)
        avg_prior = sum(v_list[:-1]) / float(len(v_list) - 1)
        if avg_prior <= 0.0:
            return 0.0
        return float(v_list[-1]) / avg_prior


EPOCH_DAY_MAP: Dict[int, int] = {}
for i, d in enumerate(TARGET_DATES):
    y, m, day = map(int, d.split("-"))
    ep = (datetime.date(y, m, day) - datetime.date(1970, 1, 1)).days
    EPOCH_DAY_MAP[ep] = i + 1  # 1-indexed day_idx (1..30)

BREAKOUT_SETUPS: Dict[int, Dict[str, Any]] = {
    1: {"sym": "TATASTEEL", "start_sec": 35100, "end_sec": 36300, "desc": "Day  1 (TATASTEEL  @ 09:45 IST)"},
    4: {"sym": "SBIN", "start_sec": 49500, "end_sec": 51000, "desc": "Day  4 (SBIN       @ 13:45 IST)"},
    12: {"sym": "ZOMATO", "start_sec": 36900, "end_sec": 38100, "desc": "Day 12 (ZOMATO     @ 10:15 IST)"},
    16: {"sym": "COALINDIA", "start_sec": 35400, "end_sec": 36600, "desc": "Day 16 (COALINDIA  @ 09:50 IST)"},
    20: {"sym": "SBIN", "start_sec": 48900, "end_sec": 50400, "desc": "Day 20 (SBIN       @ 13:35 IST)"},
    26: {"sym": "ITC", "start_sec": 35700, "end_sec": 36900, "desc": "Day 26 (ITC        @ 09:55 IST)"},
    29: {"sym": "TATASTEEL", "start_sec": 50400, "end_sec": 51600, "desc": "Day 29 (TATASTEEL  @ 14:00 IST)"},
}

CHOP_DAYS = [2, 11, 19]


def validate_dataset(csv_path: str) -> bool:
    """
    DeltaUno Genuine Microstructure & Financial Invariant Validation Oracle.
    Executes a single-pass streaming audit asserting Tier 1, Tier 2, and Tier 3 invariants:
      Tier 1: 8 low-level financial and protocol invariants
      Tier 2: Authenticated breakout trade signals on all 7 benchmark days
      Tier 3: Quietness, zero signals, bounded OFI and VolMult on all 3 chop benchmark days
    """
    print("\n" + "=" * 88)
    print(" Running DeltaUno Dataset Invariant & Microstructure Validation Oracle...")
    print("=" * 88)

    if not os.path.exists(csv_path):
        print(f"[FAIL] Output file {csv_path} does not exist!")
        return False

    val_start_time = time.time()

    expected_tokens = {
        "TATASTEEL": 349121,
        "SBIN": 779521,
        "ITC": 424961,
        "COALINDIA": 521729,
        "ZOMATO": 895745,
    }

    # Tracking state
    prev_ts = 0
    row_count = 0
    symbols_seen = set()
    symbol_daily_vols: Dict[Tuple[int, str], int] = {}
    current_epoch_day = -1
    max_inter_tick_gap_ns = 0
    watchdog_pauses_found = 0

    books: Dict[str, SimOrderBook] = {
        s: SimOrderBook(expected_tokens[s], s) for s in expected_tokens
    }
    strats: Dict[str, SimStrategy] = {
        s: SimStrategy() for s in expected_tokens
    }

    # Breakout signals recorded: day_idx -> list of signal dicts
    breakout_signals: Dict[int, List[Dict[str, Any]]] = {d: [] for d in BREAKOUT_SETUPS}

    # Chop regime metrics: day_idx -> metrics dict
    chop_metrics: Dict[int, Dict[str, Any]] = {
        d: {"signals": 0, "max_vol_mult": 0.0, "max_abs_ofi": 0.0, "ticks": 0}
        for d in CHOP_DAYS
    }

    with open(csv_path, "r", encoding="utf-8") as f:
        header = f.readline().strip()
        expected_header = "#timestamp_ns,token,symbol,last_price_inr,last_qty,volume,bid0_p,bid0_q,ask0_p,ask0_q,upper_c,lower_c"
        if header != expected_header:
            print(f"[FAIL] Header mismatch!\n  Got:      '{header}'\n  Expected: '{expected_header}'")
            return False

        for line_no, line in enumerate(f, 2):
            line = line.strip()
            if not line or line.startswith("#ALERT,"):
                continue

            parts = line.split(",")
            if len(parts) != 12:
                print(f"[FAIL] Row {line_no} does not have 12 columns: {line}")
                return False

            ts_ns = int(parts[0])
            token = int(parts[1])
            symbol = parts[2]
            ltp_inr = float(parts[3])
            last_qty = int(parts[4])
            vol = int(parts[5])
            bid0_p_inr = float(parts[6])
            bid0_q = int(parts[7])
            ask0_p_inr = float(parts[8])
            ask0_q = int(parts[9])
            upper_c_inr = float(parts[10])
            lower_c_inr = float(parts[11])

            # Convert to paise integers (exact matching C++ core::Price)
            ltp_p = int(round(ltp_inr * 100))
            bid0_p = int(round(bid0_p_inr * 100))
            ask0_p = int(round(ask0_p_inr * 100))
            upper_c_p = int(round(upper_c_inr * 100))
            lower_c_p = int(round(lower_c_inr * 100))

            # Tier 1 Invariant 1: Strictly monotonic timestamps across entire dataset
            if ts_ns <= prev_ts:
                print(f"[FAIL] Monotonicity violated at row {line_no}: ts {ts_ns} <= prev_ts {prev_ts}")
                return False

            # Tier 1 Invariant 2: Positive spread (ask0_p > bid0_p)
            if ask0_p <= bid0_p:
                print(f"[FAIL] Crossed book at row {line_no}: ask {ask0_p_inr} <= bid {bid0_p_inr}")
                return False

            # Tier 1 Invariant 3: Tick size precision (all prices exact multiples of INR 0.05 / 5 paise)
            if (ltp_p % 5 != 0 or bid0_p % 5 != 0 or ask0_p % 5 != 0 or
                    upper_c_p % 5 != 0 or lower_c_p % 5 != 0):
                print(f"[FAIL] Tick size violation at row {line_no}")
                return False

            # Tier 1 Invariant 4: Token to symbol mapping
            if symbol not in expected_tokens or expected_tokens[symbol] != token:
                print(f"[FAIL] Invalid symbol/token mapping at row {line_no}: {symbol} != {token}")
                return False

            symbols_seen.add(symbol)

            # Calendar & Session Time tracking in IST
            ts_sec_ist = (ts_ns // 1_000_000_000) + 19800
            epoch_day = ts_sec_ist // 86400
            sec_of_day = ts_sec_ist % 86400
            day_idx = EPOCH_DAY_MAP.get(epoch_day, -1)

            # Daily Session Reset across order books and strategies
            if current_epoch_day != -1 and epoch_day != current_epoch_day:
                for b in books.values():
                    b.reset()
                for s in strats.values():
                    s.reset()

            # Tier 1 Invariant 5: Monotonic volume accumulation per symbol per session
            sym_key = (epoch_day, symbol)
            if sym_key in symbol_daily_vols:
                if vol < symbol_daily_vols[sym_key]:
                    print(f"[FAIL] Volume decrease at row {line_no} for {symbol}: {vol} < {symbol_daily_vols[sym_key]}")
                    return False
            symbol_daily_vols[sym_key] = vol

            # Tier 1 Invariant 6: Inter-tick pacing gap <= 3.0s (except intentional Day 22 watchdog test pause)
            if prev_ts > 0 and epoch_day == current_epoch_day:
                gap_ns = ts_ns - prev_ts
                gap_sec = gap_ns / 1_000_000_000.0
                if 5.0 < gap_sec < 8.0:
                    watchdog_pauses_found += 1
                else:
                    if gap_ns > max_inter_tick_gap_ns:
                        max_inter_tick_gap_ns = gap_ns
                    if gap_sec > 3.0:
                        print(f"[FAIL] Inter-tick gap exceeded 3.0s at row {line_no}: {gap_sec:.2f}s")
                        return False

            # Update Microstructure State (OrderBook and Strategy)
            book = books[symbol]
            strat = strats[symbol]

            book.on_tick(ltp_p, bid0_p, bid0_q, ask0_p, ask0_q, upper_c_p, lower_c_p)
            strat.on_tick(ltp_p, last_qty, vol)

            comp_ofi = book.composite_ofi()
            vol_mult = strat.latest_volume_multiple()
            vwap_p = strat.get_vwap()
            near_circuit = book.is_near_circuit(1.5)
            above_vwap = (vwap_p == 0) or (bid0_p >= vwap_p)

            # Signal Eligibility Gate (matching MomentumBreakoutStrategy::generate_signal)
            is_valid_signal = (
                len(strat.recent_vols) >= 10
                and vol_mult >= 2.5
                and comp_ofi >= 0.55
                and above_vwap
                and not near_circuit
                and ask0_p > 0
            )

            # Tier 2 Audit: Check Breakout Targets
            if day_idx in BREAKOUT_SETUPS:
                setup = BREAKOUT_SETUPS[day_idx]
                if symbol == setup["sym"] and setup["start_sec"] <= sec_of_day <= setup["end_sec"]:
                    if is_valid_signal:
                        breakout_signals[day_idx].append({
                            "ts_ns": ts_ns,
                            "sec": sec_of_day,
                            "ltp": ltp_inr,
                            "bid0_p": bid0_p_inr,
                            "vwap": vwap_p / 100.0,
                            "vol_mult": vol_mult,
                            "ofi": comp_ofi,
                        })

            # Tier 3 Audit: Check Chop Benchmark Days (Days 2, 11, 19)
            if day_idx in CHOP_DAYS:
                # Valid trading windows: 09:30-11:00 (34200-39600s) and 13:30-14:45 (48600-53100s)
                if (34200 <= sec_of_day <= 39600) or (48600 <= sec_of_day <= 53100):
                    m = chop_metrics[day_idx]
                    m["ticks"] += 1
                    if vol_mult > m["max_vol_mult"]:
                        m["max_vol_mult"] = vol_mult
                    if abs(comp_ofi) > m["max_abs_ofi"]:
                        m["max_abs_ofi"] = abs(comp_ofi)
                    if is_valid_signal:
                        m["signals"] += 1

            prev_ts = ts_ns
            current_epoch_day = epoch_day
            row_count += 1

    unique_days = len(set(d for d, s in symbol_daily_vols.keys()))
    max_gap_sec = max_inter_tick_gap_ns / 1_000_000_000.0

    # Assert Tier 1: Low-level invariants
    if unique_days != 30:
        print(f"[FAIL] Expected 30 unique trading days, found {unique_days}")
        return False
    if len(symbols_seen) != 5:
        print(f"[FAIL] Expected 5 symbols, found {len(symbols_seen)}")
        return False
    if watchdog_pauses_found != 1:
        print(f"[FAIL] Expected exactly 1 Watchdog test pause on Day 22, found {watchdog_pauses_found}")
        return False

    # Print Tier 1 Report
    print(f" [PASS] Total valid rows checked: {row_count:,}")
    print(f" [PASS] Unique trading days: {unique_days} / 30")
    print(f" [PASS] All 5 whitelisted instruments present: {sorted(list(symbols_seen))}")
    print(f" [PASS] Strictly monotonic timestamps (t_0 < t_1 < t_2) across entire dataset")
    print(f" [PASS] 100% positive spreads (ask0_p - bid0_p >= 0.05)")
    print(f" [PASS] 100% adherence to INR 0.05 tick size precision")
    print(f" [PASS] Strict cumulative volume monotonicity within each day/symbol")
    print(f" [PASS] Max intraday tick pacing gap: {max_gap_sec:.2f}s (strictly <= 3.0s)")
    print(f" [PASS] Controlled Watchdog test pauses verified: {watchdog_pauses_found} (Day 22 6.0s pause)")

    # Assert Tier 2: Breakout Regime Invariants
    print("-" * 88)
    print(" BREAKOUT REGIME AUDIT (7 / 7 Setups Verified):")
    tier2_passed = True
    for d, setup in BREAKOUT_SETUPS.items():
        sigs = breakout_signals[d]
        if not sigs:
            print(f"  [FAIL] {setup['desc']}: 0 signals generated in target window!")
            tier2_passed = False
        else:
            first = sigs[0]
            hh = first["sec"] // 3600
            mm = (first["sec"] % 3600) // 60
            ss = first["sec"] % 60
            print(
                f"  - {setup['desc']}: PASS ({len(sigs):2d} signals | "
                f"First: {hh:02d}:{mm:02d}:{ss:02d} LTP={first['ltp']:.2f} "
                f"Bid={first['bid0_p']:.2f} VWAP={first['vwap']:.2f} "
                f"VolM={first['vol_mult']:.2f}x OFI={first['ofi']:+.3f})"
            )

    if not tier2_passed:
        print("[FAIL] Tier 2 Breakout Regime Audit FAILED!")
        return False

    # Assert Tier 3: Chop Regime Invariants
    print("-" * 88)
    print(" CHOP REGIME AUDIT (3 / 3 Benchmark Sessions Verified Flat):")
    tier3_passed = True
    for d in CHOP_DAYS:
        m = chop_metrics[d]
        cal_date = TARGET_DATES[d - 1]
        sig_ok = (m["signals"] == 0)
        vm_ok = (m["max_vol_mult"] < 2.0)
        ofi_ok = (m["max_abs_ofi"] < 0.30)
        status = "PASS" if (sig_ok and vm_ok and ofi_ok) else "FAIL"

        print(
            f"  - Day {d:2d} ({cal_date}): {status} "
            f"({m['signals']} signals | Max VolM = {m['max_vol_mult']:.2f}x < 2.0 | "
            f"Max OFI = {m['max_abs_ofi']:.3f} < 0.30 | {m['ticks']:,} ticks checked)"
        )
        if not (sig_ok and vm_ok and ofi_ok):
            tier3_passed = False

    if not tier3_passed:
        print("[FAIL] Tier 3 Chop Regime Audit FAILED!")
        return False

    elapsed = time.time() - val_start_time
    print("=" * 88)
    print(f" ALL ARCHITECTURAL, FINANCIAL & MICROSTRUCTURE INVARIANTS SATISFIED AT 100% ({elapsed:.2f}s)\n")
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="DeltaUno 30-Day Market Data Generator")
    parser.add_argument("--out", default="data/historical_30days_nse.csv", help="Output CSV path")
    parser.add_argument("--cache-dir", default="data/cache_1m", help="Raw bar cache directory")
    parser.add_argument("--offline", action="store_true", help="Force offline calibrated synthetic generation")
    parser.add_argument("--validate", action="store_true", default=True, help="Validate dataset invariants")
    args = parser.parse_args()

    generate_30day_dataset(args.out, args.cache_dir, args.offline)

    if args.validate:
        if not validate_dataset(args.out):
            sys.exit(1)
