# DeltaUno: Low-Latency C++ Trading Bot Engine

A modular, high-performance **C++20 algorithmic trading engine** engineered for capital preservation and disciplined trickle compounding on a micro-account (**₹1,000 / ~$10**).

Built for **Indian Equities (NSE/BSE)** with multi-asset extensibility, grounded in empirical market microstructure research.

---

## 🏛️ System Architecture

```
[ Market Feed (CSV / WebSocket) ]          [ External Trader Webhook ]
               │                                      │
               ▼                                      ▼
[ SPSC Lock-Free Ring Buffer ]            [ Signal Ingestor ("BS Detector") ]
               │                                      │
               ├──────────────────────────────────────┘
               ▼
[ L2 Order Book & State Engine ]
   ├── Depth-Normalized OFI (Cont et al. 2014)
   └── Multi-Horizon Exponential OFI (Kolm et al. 2023)
               │
               ▼
[ Momentum Breakout Strategy ]
   ├── Volume Surge Acceleration (> 2.0x 20-MA)
   └── VWAP Alignment & Whitelist Filter
               │
               ▼
[ Pre-Trade Risk Manager (13 Shields) ]
   ├── Dynamic Sizing (<= ₹12 risk cap per trade)
   ├── Daily Drawdown Kill-Switch (₹50 / 5%)
   ├── Zero-Loss Break-Even Trailing Stop (+1.0%)
   ├── NSE Auto-Squareoff Shield (15:05 IST Cutoff)
   ├── Circuit Band Buffer (1.5% distance check)
   └── Statutory Friction Filter (Gain > 2.5x Fees)
               │
               ▼
[ Paper Broker / Execution Gateway ]
   ├── Zero Look-Ahead Bias (Tick[t] fills on Tick[t+1])
   ├── Almgren-Chriss Non-Linear Slippage Model
   └── Penny-Accurate Indian Statutory Tax Deductions
               │
               ▼
[ Diagnostic Auditor & WAL State Journal ]
   ├── MAE / MFE & Friction Drag Ratio Analytics
   └── Crash Recovery Journal (tradeuno_state.json)
```

---

## 📚 Grounded in Quantitative Research

| Academic Research Paper | Authors & Year | Implemented Feature in TradeUno |
| :--- | :--- | :--- |
| **The Price Impact of Order Book Events** | Rama Cont, Arseniy Kukanov, Sasha Stoikov (*J. Fin. Econometrics*, 2014) | **Depth-Normalized OFI**: Measures net order flow pressure ($\Delta \text{Bid} - \Delta \text{Ask}$) normalized by top-5 depth. |
| **Deep Order Flow Imbalance** | Petter N. Kolm, Jeremy Turiel, Nicholas Westray (*Math. Finance*, 2023) | **Multi-Horizon Exponential Decay**: Combines $OFI_1$, $OFI_5$, and $OFI_{20}$ to eliminate spoofing noise and capture short-horizon micro-bursts. |
| **About the Risks of Social & Copy-Trading** | Matthias Pelster, Annette Hofmann (*J. Banking & Finance*, 2018) | **"BS Detector" & Overcrowding Guard**: Rejects external trader alerts if local order book shows selling pressure or if volume has already surged $> 4\times$. |
| **Optimal Execution of Portfolio Transactions** | Robert Almgren, Neil Chriss (*J. Risk*, 2000) | **Friction & Non-Linear Slippage Engine**: Simulates impact based on order size vs. book depth; rejects trades where gain $< 2.5\times$ fees. |
| **Time Series Momentum** | Tobias J. Moskowitz, Yao Hua Ooi, Lasse Heje Pedersen (*JFE*, 2012) | **Volatility Position Sizing**: Equalizes risk so each position risks exactly $\le \text{₹}12.00$ regardless of share price. |

---

## 🛡️ The 13 Micro-Risk & Operational Safeguards

1. **Fixed-Risk Position Sizing**:
   $$Quantity = \min\left(\left\lfloor \frac{\text{₹}12.00}{\text{Entry} - \text{SL}} \right\rfloor, \left\lfloor \frac{\text{₹}500.00}{\text{Entry}} \right\rfloor\right)$$
2. **Daily Drawdown Kill-Switch**: Hard stop disables all orders if daily loss reaches **₹50.00 (5%)**.
3. **Zero-Loss Break-Even Trailing**: When profit $\ge +1.0\%$, Stop Loss moves to Entry + 0.15% (taxes covered).
4. **Auto-Squareoff Shield**: Automatically closes intraday positions at **15:05 IST**, completely avoiding broker auto-squareoff penalty fees (₹50+GST).
5. **Circuit Band Guard**: Vetoes orders within 1.5% of Upper or Lower Circuit to avoid 0-liquidity lockouts.
6. **Time-of-Day Filter**: Restricts entries strictly to high-probability windows (**09:30–11:00** and **13:30–14:45 IST**).
7. **Zero Look-Ahead Bias**: Orders placed on $Tick[t]$ fill on $Tick[t+1]$ at opposite book prices.
8. **Crash Recovery Journal**: State saved to `tradeuno_state.json` (zero orphaned positions across reboots).
9. **Async Lock-Free Logger**: Ring-buffer logger pushes I/O to background thread with zero tick-to-trade jitter.
10. **Network Watchdog**: Raises emergency alerts if ticks stall $> 5\text{s}$ while holding a position.
11. **Macro Event Freeze**: Rejects entries during scheduled RBI, Budget, or FOMC event windows.
12. **Statutory Tax Pre-Check**: Simulates STT (0.025%), exchange fee (0.00345%), GST (18%), stamp duty (0.003%), and SEBI fees before entry.
13. **Post-Trade Auditor**: Computes MAE (Maximum Adverse Excursion), MFE (Maximum Favorable Excursion), and Friction Drag Ratio after every session.

---

## 📂 Project Structure

```
tradeUno/
├── CMakeLists.txt                 # C++20 build configuration
├── README.md                      # Master architecture documentation
├── .gitignore
├── config/
│   ├── risk_limits.json           # ₹1,000 capital limits & cutoff times
│   ├── instruments.json           # Whitelist of liquid NSE stocks under ₹1,000
│   └── calendar_events.json       # Macro event freeze schedule (RBI, budget)
├── include/
│   └── tradeuno/
│       ├── core/
│       │   ├── types.hpp          # Fixed-point Price, MarketTick, Order, TradeAudit
│       │   ├── ring_buffer.hpp    # Cache-aligned lock-free SPSC queue
│       │   ├── async_logger.hpp   # Non-blocking async logger
│       │   └── state_journal.hpp  # Write-Ahead crash recovery journal
│       ├── market/
│       │   ├── order_book.hpp     # Fast L2 depth book & OFI calculation
│       │   ├── feed_interface.hpp # Base feed interface
│       │   ├── csv_feed.hpp       # Deterministic tick replayer
│       │   └── watchdog.hpp       # Network heartbeat watchdog timer
│       ├── strategy/
│       │   ├── strategy_interface.hpp
│       │   ├── momentum_breakout.hpp # Volume acceleration & micro-breakout
│       │   └── signal_ingestor.hpp   # Alert receiver with "BS detector"
│       ├── risk/
│       │   └── risk_manager.hpp   # All 13 risk guards & trailing SL
│       └── execution/
│           ├── execution_interface.hpp
│           ├── paper_broker.hpp   # Matching engine with STT/GST/Slippage
│           └── trade_auditor.hpp  # MAE, MFE, Friction Drag Ratio calculator
├── src/
│   └── main.cpp                   # Main engine orchestrator
├── data/
│   ├── sample_nse_ticks.csv       # Sample high-frequency NSE market depth data
│   └── chop_ticks.csv             # Simulated rangebound noise dataset
├── scripts/
│   └── generate_nse_ticks.py      # Scenario generator (momentum, chop, circuit)
└── tests/
    ├── test_order_book.cpp        # Depth, OFI & circuit limit unit tests
    ├── test_risk_manager.cpp      # Sizing, kill-switch & trailing stop unit tests
    ├── test_state_journal.cpp     # Crash recovery & WAL serialization tests
    └── test_paper_broker.cpp      # Zero look-ahead & statutory tax tests
```

---

## 🚀 Building & Running

### Prerequisites
* CMake 3.20+
* C++20 compliant compiler (Clang / GCC MinGW / MSVC)

### Build All Targets
```powershell
# Set environment PATH (if newly installed):
$env:PATH = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")

# Configure & Build
cmake -G "MinGW Makefiles" -B build
cmake --build build
```

### Run Automated Unit Tests (100% Pass)
```powershell
cd build
ctest --output-on-failure
cd ..
```

### Run Backtest Simulation
```powershell
.\build\tradeuno.exe data/sample_nse_ticks.csv
```

### Generate Custom Market Scenarios
```powershell
# Momentum Breakout Scenario
python scripts/generate_nse_ticks.py --symbol TATASTEEL --scenario momentum --out data/sample_nse_ticks.csv

# Choppy Sideways Rangebound Scenario (Verifies bot refuses to trade)
python scripts/generate_nse_ticks.py --symbol TATASTEEL --scenario chop --out data/chop_ticks.csv

# Upper Circuit Scenario (Verifies circuit band safety veto)
python scripts/generate_nse_ticks.py --symbol TATASTEEL --scenario circuit --out data/circuit_ticks.csv
```
