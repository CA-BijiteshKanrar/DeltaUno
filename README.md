# TradeUno: Low-Latency C++ Trading Bot Engine

A high-performance C++20 algorithmic trading engine engineered for capital preservation and disciplined trickle compounding on micro-capital (**₹1,000 / ~$10**). 

Engineered for **Indian Equities (NSE/BSE)** with multi-asset extensibility, grounded in empirical market microstructure research.

---

## 🏛️ Architectural Foundations

TradeUno is built upon 4 seminal quantitative principles:
1. **Order Flow Imbalance (OFI)**: Calculates continuous depth pressure ($\Delta \text{BidSize} - \Delta \text{AskSize}$) normalized by top-5 depth (*Cont, Kukanov, Stoikov 2014*).
2. **Multi-Horizon Exponential Decay**: Combines $OFI_1$, $OFI_5$, and $OFI_{20}$ to filter out single-tick order spoofing (*Kolm, Turiel, Westray 2023*).
3. **"BS Detector" & Overcrowding Guard**: Cross-references external trader alerts against local order book depth and volume velocity to prevent buying into exit-traps (*Pelster & Hofmann 2018*).
4. **Almgren-Chriss Slippage & Exact Statutory Friction**: Simulates non-linear order impact and deducts penny-accurate Indian statutory charges (STT, GST, turnover, stamp duty) before trade confirmation (*Almgren & Chriss 2000*).

---

## 🛡️ The 13 Micro-Risk & Operational Safeguards

* **Fixed-Risk Sizing**: Single trade loss strictly capped at $\le \text{₹}12.00$ ($Quantity = \min(\lfloor \frac{12}{\text{Entry}-\text{SL}} \rfloor, \lfloor \frac{500}{\text{Entry}} \rfloor)$).
* **Daily Drawdown Kill-Switch**: Halts all operations for the remainder of the day if daily drawdown hits **₹50.00 (5%)**.
* **Zero-Loss Break-Even Trailing**: When trade reaches $+1.0\%$ profit, stop-loss ratchets to Entry + 0.15% (fees covered), ensuring winning trades never become losers.
* **Auto-Squareoff Shield**: Forcibly closes all intraday positions at **15:05 IST**, avoiding broker forced-squareoff penalties (₹50+GST).
* **Circuit Band Guard**: Vetoes orders within 1.5% of Upper or Lower Circuit to avoid 0-liquidity traps.
* **Time-of-Day Filter**: Restricts entries strictly to **09:30–11:00** and **13:30–14:45 IST** (avoids midday chop and opening range traps).
* **Zero Look-Ahead Bias**: Orders triggered on $Tick[t]$ fill on $Tick[t+1]$ at opposite book prices.
* **Crash Recovery Journal**: State saved to `tradeuno_state.json` (zero orphaned positions across reboots).
* **Async Non-Blocking Logger**: Lock-free logger offloads I/O to background thread with zero tick jitter.
* **Network Watchdog**: Raises emergency alerts if ticks stall $> 5\text{s}$ while holding a position.
* **Diagnostic Auditor**: Computes MAE (Maximum Adverse Excursion), MFE (Maximum Favorable Excursion), and Friction Drag Ratio after every session.

---

## 🚀 Building & Running

### Prerequisites
* CMake 3.20+
* C++20 compliant compiler (Clang / GCC MinGW / MSVC)

### Build
```powershell
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

### Run Unit Tests
```powershell
ctest --output-on-failure
# Or run executables directly:
./test_order_book
./test_risk_manager
./test_state_journal
./test_paper_broker
```

### Run Backtest Simulation
```powershell
./tradeuno data/sample_nse_ticks.csv
```

### Generate Custom Market Scenarios
```powershell
python scripts/generate_nse_ticks.py --symbol TATASTEEL --scenario momentum --out data/sample_nse_ticks.csv
python scripts/generate_nse_ticks.py --symbol SBIN --scenario chop --out data/chop_ticks.csv
```
