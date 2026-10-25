# DeltaUno: Low-Latency C++20 Trading Bot Engine

A modular, high-performance **C++20 algorithmic trading engine** engineered for capital preservation and disciplined trickle compounding on a ring-fenced pilot account (**₹10,000 / ~\$115**).

Built for **Indian Equities (NSE/BSE)** with ₹0.05 tick-size precision and multi-asset extensibility, grounded in empirical market microstructure research.

---

## 🏛️ System Architecture

```text
[ Market Feed (CSV / WebSocket) ]          [ External Trader Webhook / Alert ]
               │                                           │
               ▼                                           ▼
[ Lock-Free SPSC Tick Ring Buffer ]        [ Lock-Free SPSC Alert Ring Buffer ]
               │                                           │
               ▼                                           ▼
[ L2 Order Book & Microstructure ] ──────► [ Signal Ingestor ("BS Detector") ]
   ├── Multi-Level Depth OFI (Cont 2014)      ├── Local OFI >= 0.20 Check
   ├── Multi-Horizon Blend (Kolm 2023)        └── >2% Price / >4x Vol Trap Veto
   └── Weighted Microprice & Spread                        │
               │                                           │
               ▼                                           │
[ Momentum Breakout Strategy ]                             │
   ├── Volume Surge Acceleration (>= 2.5x 20-MA)           │
   └── Session VWAP & Liquid Whitelist Filter              │
               │                                           │
               ├───────────────────────────────────────────┘
               ▼
[ Pre-Trade Risk Manager (13 Institutional Shields) ]
   ├── Dynamic Fixed-Risk Sizing (<= ₹120 risk & <= ₹5,000 notional)
   ├── Net / Peak-to-Trough Daily Drawdown Kill-Switch (₹500 / 5%)
   ├── Zero-Loss Break-Even Trailing Stop (+1.0% -> Entry + 0.15%)
   ├── NSE Auto-Squareoff Shield (15:05 IST Cutoff)
   ├── Circuit Band Guard (1.5% distance check)
   ├── Macro Event Freeze (RBI / Budget / FOMC Calendar)
   └── Statutory Friction Filter (Expected Gain >= 2.5x Fees)
               │
               ▼
[ Paper Broker / Execution Gateway ]
   ├── Strict Zero Look-Ahead Bias (Tick[t] fills on Tick[t+1])
   ├── Almgren-Chriss Non-Linear Adverse Slippage Model
   └── Penny-Accurate NSE Statutory Charges (STT, Turnover, GST, Stamp, SEBI)
               │
               ▼
[ Diagnostic Auditor, Watchdog & Atomic WAL State Journal ]
   ├── MAE / MFE Excursion & Exact PnL Friction Drag Analytics
   ├── Dual-Mode Feed Watchdog (Steady-Clock + Replay Tick-Gap Monitor)
   ├── Lock-Free MPMC Ring-Buffer Async Logger (deltauno.log)
   └── Atomic Crash Recovery Journal (deltauno_state.json)
```

---

## 📚 Grounded in Quantitative Research (10 Peer-Reviewed Works)

| # | Academic Research Paper | Authors, Venue & Year | How DeltaUno Implements / Compares |
| :---: | :--- | :--- | :--- |
| **1** | **The Price Impact of Order Book Events** | Cont, Kukanov & Stoikov (*J. Financial Econometrics*, 2014) | **Adopted — Multi-Level Depth OFI**: Measures weighted queue pressure ($\Delta \text{Bid}_m - \Delta \text{Ask}_m$) across top-5 NSE L2 levels normalized by book depth ($R^2 \approx 65\%$). |
| **2** | **Deep Order Flow Imbalance** | Kolm, Turiel & Westray (*Mathematical Finance*, 2023) | **Adapted — Multi-Horizon Blend**: Combines stationary level weights (`{1.0, 0.7, 0.5, 0.35, 0.25}`) with multi-horizon decay ($0.5\,\text{OFI}_1 + 0.3\,\text{OFI}_5 + 0.2\,\text{OFI}_{20}$) using auditable fixed weights instead of neural layers. |
| **3** | **DeepLOB: Deep CNNs for Limit Order Books** | Zhang, Zohren & Roberts (*IEEE Trans. Signal Processing*, 2019) | **Deliberate Divergence — Zero Black-Box Drift**: Rejects GPU-heavy CNN-LSTM weights prone to regime overfitting in favor of 100% explainable analytical OFI + deterministic pre-trade gates. |
| **4** | **The Micro-Price: High-Frequency Estimator** | Sasha Stoikov (*Quantitative Finance*, 2018) | **Adopted — Queue-Weighted Fair Value**: Computes queue-imbalance-weighted microprice (`OrderBook::weighted_mid_price`) alongside session VWAP alignment (`best_bid >= VWAP`). |
| **5** | **Market Intraday Momentum** | Gao, Han, Li & Zhou (*J. Financial Economics*, 2018) | **Adopted — Institutional Window Filter**: Restricts entries to high-volume morning (`09:30–11:00 IST`) and closing (`13:30–14:45 IST`) windows, blocking `11:00–13:30 IST` midday chop. |
| **6** | **Flow Toxicity and Liquidity in an HFT World** | Easley, López de Prado & O'Hara (*Rev. Financial Studies*, 2012) | **Mitigated — Shock & Toxicity Shields**: Enforces Macro Event Freeze (RBI/Budget/FOMC windows), `1.5%` Circuit Band Guard, and a `50%` unencumbered cash reserve (`<= ₹5,000` notional). |
| **7** | **About the Risks of Social & Copy-Trading** | Pelster & Hofmann (*J. Banking & Finance*, 2018) | **Countered — "BS Detector" & Breakeven Stop**: Vetoes external webhooks when local `OFI < 0.20` or price pumped `> 2%`; automated `-1.0%` SL and `+1.0%` breakeven trailing eliminate disposition bias. |
| **8** | **Attention-Induced Trading and Returns** | Barber, Huang, Odean & Schwarz (*Journal of Finance*, 2022) | **Countered — Anti-Herding Guard**: Restricts universe to 5 liquid Nifty large-caps, blocks `> 4.0x` volume-climax spikes, and bans penny/attention stocks even on zero-brokerage gateways. |
| **9** | **Optimal Execution of Portfolio Transactions** | Almgren & Chriss (*Journal of Risk*, 2000) | **Adopted — Adverse Slippage & Fee Gate**: Models `ceil`/`floor` market impact scaled by order-to-depth ratio on `Tick[t+1]` and vetoes setups with `Expected Gain < 2.5x Statutory Fees`. |
| **10** | **Time Series Momentum** | Moskowitz, Ooi & Pedersen (*J. Financial Economics*, 2012) | **Adopted — Fixed-Risk Volatility Sizing**: Scales share quantity inversely to stop distance so per-trade risk never exceeds `₹120.00` (`1.20%` of capital) across regimes. |

### Academic References Cited
1. Cont, R., Kukanov, A., & Stoikov, S. (2014). *The Price Impact of Order Book Events*. Journal of Financial Econometrics, 12(1), 47–88.
2. Kolm, P. N., Turiel, J., & Westray, N. (2023). *Deep Order Flow Imbalance: Extracting Alpha at Multiple Horizons from the Limit Order Book*. Mathematical Finance, 33(4), 1044–1081.
3. Zhang, Z., Zohren, S., & Roberts, S. (2019). *DeepLOB: Deep Convolutional Neural Networks for Limit Order Books*. IEEE Transactions on Signal Processing, 67(11), 3001–3012.
4. Stoikov, S. (2018). *The Micro-Price: A High-Frequency Estimator of Future Prices*. Quantitative Finance, 18(12), 1959–1966.
5. Gao, L., Han, Y., Li, S. Z., & Zhou, G. (2018). *Market Intraday Momentum*. Journal of Financial Economics, 129(2), 394–414.
6. Easley, D., López de Prado, M. M., & O'Hara, M. (2012). *Flow Toxicity and Liquidity in a High-Frequency World*. Review of Financial Studies, 25(5), 1457–1493.
7. Pelster, M., & Hofmann, A. (2018). *About the Risks of Social Trading: Copy-Trading and the Disposition Effect*. Journal of Banking & Finance, 94, 75–88.
8. Barber, B. M., Huang, X., Odean, T., & Schwarz, C. G. (2022). *Attention-Induced Trading and Returns: Evidence from Robinhood Users*. Journal of Finance, 77(6), 3141–3190.
9. Almgren, R., & Chriss, N. (2000). *Optimal Execution of Portfolio Transactions*. Journal of Risk, 3(2), 5–39.
10. Moskowitz, T. J., Ooi, Y. H., & Pedersen, L. H. (2012). *Time Series Momentum*. Journal of Financial Economics, 104(2), 228–250.

---

## 🛡️ The 13 Risk & Operational Safeguards

1. **Fixed-Risk Position Sizing**:
   $$\text{Quantity} = \min\left(\left\lfloor \frac{\text{₹}120.00}{\text{Entry} - \text{SL}} \right\rfloor, \left\lfloor \frac{\text{₹}5,000.00}{\text{Entry}} \right\rfloor\right)$$
2. **Daily Drawdown Kill-Switch**: Hard cutoff disables new entries if intraday net or peak-to-trough drawdown reaches **₹500.00 (5%)**, with automatic reset on IST day rollover.
3. **Zero-Loss Break-Even Trailing**: Once unrealized gain reaches **+1.0%**, Stop Loss ratchets to `ceil_to_tick(Entry + 0.15%, ₹0.05)` so all statutory taxes are covered.
4. **NSE Auto-Squareoff Shield**: Forcibly exits open intraday positions at **15:05 IST**, avoiding ₹50 + GST broker auto-squareoff penalties.
5. **Circuit Band Guard**: Vetoes entries within **1.5%** of Upper or Lower Circuit limits in both strategy and `RiskManager`.
6. **Time-of-Day Filter**: Restricts entries to high-liquidity windows (**09:30–11:00** and **13:30–14:45 IST**).
7. **Zero Look-Ahead Bias**: Orders submitted on $\text{Tick}[t]$ match strictly on $\text{Tick}[t+1]$ (`timestamp_ns > created_time_ns`) at opposite book prices, guarded by an explicit `FLAT → PENDING_ENTRY → OPEN → PENDING_EXIT` state machine.
8. **Atomic Crash Recovery Journal**: Writes open position state via `.tmp` + atomic rename (`deltauno_state.json`) and restores `Position`, `RiskManager`, `PaperBroker`, and `Watchdog` on startup.
9. **Lock-Free Async Logger**: Bounded MPMC ring-buffer logger formats timestamps off the hot thread using C++20 atomic wait/notify (`deltauno.log`).
10. **Dual-Mode Network Watchdog**: Monitors live `steady_clock` stalls and deterministic replay tick-timestamp gaps ($> 5\text{s}$) whenever a position is open.
11. **Macro Event Freeze**: Precomputes UTC nanosecond windows from `config/calendar_events.json` to block entries during RBI, Union Budget, or FOMC announcements in $O(1)$ integer comparisons.
12. **Statutory Tax Pre-Check**: Pre-calculates STT (0.025%), NSE turnover (0.00345%), GST (18%), stamp duty (0.003%), and SEBI turnover fees (0.0001%) prior to order submission.
13. **Post-Trade Diagnostic Auditor**: Tracks tick-by-tick MAE (Maximum Adverse Excursion), MFE (Maximum Favorable Excursion), Win Rate, Profit Factor, and exact Friction Drag Ratio.

---

## 📂 Project Structure

```text
DeltaUno/
├── CMakeLists.txt                       # C++20 build & 6-suite CTest configuration
├── README.md                            # Master architecture & usage guide
├── .gitignore
├── config/
│   ├── risk_limits.json                 # ₹10,000 capital limits, windows & cutoffs
│   ├── instruments.json                 # Whitelist of liquid NSE stocks (₹0.05 tick)
│   └── calendar_events.json             # Macro event freeze schedule (RBI, Budget, FOMC)
├── docs/
│   ├── financial_governance_approval.md # Executive risk & capital sign-off charter
│   ├── implementation_plan.md           # Detailed system design & verification spec
│   └── walkthrough.md                   # Build, CTest & simulation verification report
├── include/
│   └── deltauno/
│       ├── core/
│       │   ├── types.hpp                # Fixed-point Price, 5p tick helpers, POD structs
│       │   ├── ring_buffer.hpp          # Cache-aligned lock-free SPSC & MPMC queues
│       │   ├── async_logger.hpp         # Lock-free MPMC async logger (off-thread formatting)
│       │   ├── state_journal.hpp        # Atomic .tmp + rename crash recovery journal
│       │   └── config_loader.hpp        # Runtime JSON loader & IST civil-time converter
│       ├── market/
│       │   ├── order_book.hpp           # 5-level L2 book, multi-level OFI & microprice
│       │   ├── feed_interface.hpp       # Market feed & external alert callback interfaces
│       │   ├── csv_feed.hpp             # Deterministic tick & #ALERT stream replayer
│       │   └── watchdog.hpp             # Dual-mode steady-clock & tick-gap watchdog
│       ├── strategy/
│       │   ├── strategy_interface.hpp
│       │   ├── momentum_breakout.hpp    # Volume surge (>= 2.5x), OFI (>= 0.55) & VWAP
│       │   └── signal_ingestor.hpp      # JSON webhook parser & L2 "BS Detector"
│       ├── risk/
│       │   └── risk_manager.hpp         # All 13 pre-trade & open-position risk shields
│       └── execution/
│           ├── execution_interface.hpp
│           ├── paper_broker.hpp         # Tick[t+1] matching, slippage, taxes & MAE/MFE
│           └── trade_auditor.hpp        # Exact PnL identity & excursion report generator
├── src/
│   └── main.cpp                         # Lock-free SPSC pipeline & engine orchestrator
├── data/
│   ├── sample_nse_ticks.csv             # Deterministic NSE momentum breakout L2 stream
│   └── chop_ticks.csv                   # Sideways rangebound noise dataset
├── scripts/
│   └── generate_nse_ticks.py            # Seeded NSE ₹0.05-tick scenario generator
└── tests/
    ├── test_macros.hpp                  # Release-safe (-DNDEBUG) test assertion macros
    ├── test_order_book.cpp              # L2 depth, OFI, circuit & SPSC concurrency tests
    ├── test_signal_ingestor.cpp         # Webhook JSON & BS Detector veto unit tests
    ├── test_risk_manager.cpp            # Sizing, drawdown, rollover, windows & macro tests
    ├── test_state_journal.cpp           # Atomic WAL crash recovery & Watchdog tests
    ├── test_paper_broker.cpp            # Tick[t+1], slippage, taxes & MAE/MFE tests
    └── test_trade_auditor.cpp           # Exact PnL identity & scratch-trade audit tests
```

---

## 🚀 Building & Running

### Prerequisites
- CMake 3.20+
- C++20 compliant compiler (Clang / GCC MinGW / MSVC)

### Build All Targets
```powershell
cmake -G "MinGW Makefiles" -B build
cmake --build build -j 4
```

### Run Automated Unit Tests (6/6 Suites)
```powershell
ctest --test-dir build --output-on-failure
```

### Run Backtest Simulation
```powershell
# Default momentum breakout backtest
.\build\deltauno.exe data/sample_nse_ticks.csv

# Choppy sideways market (verifies bot stays flat with 0 fee bleed)
.\build\deltauno.exe data/chop_ticks.csv

# Test external trader webhook validation through the local L2 "BS Detector"
.\build\deltauno.exe data/chop_ticks.csv --webhook "{\"symbol\":\"PENNYPUMP\",\"side\":\"BUY\",\"entry\":12.00,\"sl\":11.50}"
```

### Generate Custom Market Scenarios
```powershell
# Deterministic Momentum Breakout Scenario
python scripts/generate_nse_ticks.py --symbol TATASTEEL --scenario momentum --seed 42 --out data/sample_nse_ticks.csv

# Choppy Sideways Rangebound Scenario
python scripts/generate_nse_ticks.py --symbol TATASTEEL --scenario chop --seed 42 --out data/chop_ticks.csv

# Upper Circuit Lockout Scenario (verifies 1.5% circuit band safety veto)
python scripts/generate_nse_ticks.py --symbol TATASTEEL --scenario circuit --seed 42 --out data/circuit_ticks.csv
```
