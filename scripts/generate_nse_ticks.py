#!/usr/bin/env python3
"""
TradeUno Market Data Generator
Generates realistic high-frequency Level 2 tick streams for NSE/BSE equities.
Scenarios:
  - momentum: High-volume breakout with positive OFI (profitable long setup)
  - chop: Low-volume rangebound sideways movement (tests time-of-day and fee filters)
  - circuit: Explosive surge toward Upper Circuit (tests circuit band guard)
"""

import sys
import argparse
import random

def generate_ticks(symbol="TATASTEEL", token=349121, base_price=150.0, scenario="momentum", output_file="data/sample_nse_ticks.csv"):
    ticks = []
    # Base timestamp: 09:45:00 IST on Nov 04, 2026 (Active morning trading window)
    start_ts_ns = 1793765700000000000
    
    current_price = base_price
    volume = 500000
    upper_c = base_price * 1.10
    lower_c = base_price * 0.90
    
    ticks.append("#timestamp_ns,token,symbol,last_price_inr,last_qty,volume,bid0_p,bid0_q,ask0_p,ask0_q,upper_c,lower_c")
    
    n_ticks = 60
    for i in range(n_ticks):
        ts = start_ts_ns + (i * 1_000_000_000) # 1 sec step
        
        if scenario == "momentum":
            if i < 10:
                # Normal warmup
                delta = random.choice([-0.05, 0.0, 0.05])
                qty = random.randint(100, 300)
                spread = 0.05
                bid_q = random.randint(500, 800)
                ask_q = random.randint(500, 800)
            elif i < 25:
                # Volume surge & aggressive buyer expansion
                delta = random.choice([0.10, 0.15, 0.20])
                qty = random.randint(1500, 3500)
                spread = 0.05
                bid_q = random.randint(2500, 5000) # Heavy bids (positive OFI)
                ask_q = random.randint(150, 400)   # Thin asks
            else:
                # Continuation & consolidation
                delta = random.choice([0.05, 0.10, -0.05])
                qty = random.randint(500, 1200)
                spread = 0.05
                bid_q = random.randint(1200, 2000)
                ask_q = random.randint(800, 1500)
        elif scenario == "circuit":
            # Rapid push directly to upper circuit
            delta = 0.50
            qty = random.randint(2000, 4000)
            spread = 0.05
            bid_q = random.randint(5000, 10000)
            ask_q = random.randint(50, 100)
        else: # chop
            delta = random.choice([-0.05, 0.05, 0.0, -0.05, 0.05])
            qty = random.randint(50, 150)
            spread = 0.10
            bid_q = random.randint(300, 500)
            ask_q = random.randint(300, 500)
            
        current_price = round(max(lower_c + 0.1, min(upper_c - 0.1, current_price + delta)), 2)
        volume += qty
        
        bid0_p = round(current_price - spread / 2, 2)
        ask0_p = round(current_price + spread / 2, 2)
        
        line = f"{ts},{token},{symbol},{current_price:.2f},{qty},{volume},{bid0_p:.2f},{bid_q},{ask0_p:.2f},{ask_q},{upper_c:.2f},{lower_c:.2f}"
        ticks.append(line)
        
    with open(output_file, "w") as f:
        f.write("\n".join(ticks) + "\n")
        
    print(f"Generated {len(ticks)-1} ticks for {symbol} under scenario '{scenario}' -> {output_file}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="TradeUno Tick Stream Generator")
    parser.add_argument("--symbol", default="TATASTEEL")
    parser.add_argument("--price", type=float, default=150.0)
    parser.add_argument("--scenario", choices=["momentum", "chop", "circuit"], default="momentum")
    parser.add_argument("--out", default="data/sample_nse_ticks.csv")
    args = parser.parse_args()
    
    generate_ticks(args.symbol, 349121, args.price, args.scenario, args.out)
