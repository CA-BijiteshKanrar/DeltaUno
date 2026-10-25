#!/usr/bin/env python3
"""
DeltaUno Market Data Generator
Generates deterministic, tick-aligned Level 2 market streams for NSE/BSE equities (INR 0.05 tick size).
Scenarios:
  - momentum: High-volume breakout with positive OFI (profitable long setup)
  - chop: Low-volume rangebound sideways movement (verifies bot stays flat and preserves capital)
  - circuit: Explosive surge toward Upper Circuit (verifies 1.5% circuit band safety veto)
"""

import argparse
import random


def round_nse_tick(price: float, tick_size: float = 0.05) -> float:
    return round(round(price / tick_size) * tick_size, 2)


def generate_ticks(
    symbol: str = "TATASTEEL",
    token: int = 349121,
    base_price: float = 150.0,
    scenario: str = "momentum",
    output_file: str = "data/sample_nse_ticks.csv",
    seed: int = 42,
) -> None:
    rng = random.Random(seed)
    ticks = []
    # Base timestamp: 09:45:00 IST on Nov 04, 2026 (Active morning trading window)
    start_ts_ns = 1793765700000000000

    current_price = round_nse_tick(base_price)
    volume = 500000
    upper_c = round_nse_tick(base_price * 1.10)
    lower_c = round_nse_tick(base_price * 0.90)

    ticks.append(
        "#timestamp_ns,token,symbol,last_price_inr,last_qty,volume,bid0_p,bid0_q,ask0_p,ask0_q,upper_c,lower_c"
    )

    n_ticks = 60
    for i in range(n_ticks):
        ts = start_ts_ns + (i * 1_000_000_000)  # 1 second step

        if scenario == "momentum":
            if i < 11:
                # Normal warmup around opening equilibrium
                delta = rng.choice([-0.05, 0.0, 0.05])
                qty = rng.randint(150, 280)
                bid_q = rng.randint(550, 750)
                ask_q = rng.randint(550, 750)
            elif i < 28:
                # Institutional volume surge & aggressive buyer queue expansion
                delta = rng.choice([0.10, 0.15, 0.20])
                qty = rng.randint(1800, 3400)
                bid_q = rng.randint(3000, 5200)  # Heavy bid support (positive OFI)
                ask_q = rng.randint(150, 350)    # Depleted asks
            else:
                # Continuation with realistic minor pullbacks before target hit
                delta = rng.choice([0.10, 0.05, -0.05, 0.10])
                qty = rng.randint(600, 1200)
                bid_q = rng.randint(1300, 2100)
                ask_q = rng.randint(800, 1400)
            spread_ticks = 1
        elif scenario == "circuit":
            # Start within 1.2% of upper circuit and push into the lock zone
            if i == 0:
                current_price = round_nse_tick(upper_c * 0.988)
            delta = 0.10
            qty = rng.randint(2500, 4500)
            bid_q = rng.randint(6000, 10000)
            ask_q = rng.randint(50, 100)
            spread_ticks = 1
        else:  # chop
            delta = rng.choice([-0.05, 0.05, 0.0, -0.05, 0.05])
            qty = rng.randint(60, 140)
            bid_q = rng.randint(320, 480)
            ask_q = rng.randint(320, 480)
            spread_ticks = 2

        current_price = round_nse_tick(
            max(lower_c + 0.10, min(upper_c - 0.05, current_price + delta))
        )
        volume += qty

        bid0_p = round_nse_tick(current_price - 0.05 * (spread_ticks // 2))
        ask0_p = round_nse_tick(bid0_p + 0.05 * spread_ticks)

        line = (
            f"{ts},{token},{symbol},{current_price:.2f},{qty},{volume},"
            f"{bid0_p:.2f},{bid_q},{ask0_p:.2f},{ask_q},{upper_c:.2f},{lower_c:.2f}"
        )
        ticks.append(line)

    with open(output_file, "w", encoding="utf-8") as f:
        f.write("\n".join(ticks) + "\n")

    print(
        f"Generated {len(ticks) - 1} NSE ticks for {symbol} under scenario '{scenario}' -> {output_file}"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="DeltaUno Tick Stream Generator")
    parser.add_argument("--symbol", default="TATASTEEL")
    parser.add_argument("--token", type=int, default=349121)
    parser.add_argument("--price", type=float, default=150.0)
    parser.add_argument("--scenario", choices=["momentum", "chop", "circuit"], default="momentum")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--out", default="data/sample_nse_ticks.csv")
    args = parser.parse_args()

    generate_ticks(args.symbol, args.token, args.price, args.scenario, args.out, args.seed)
