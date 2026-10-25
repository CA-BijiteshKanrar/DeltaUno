# DeltaUno: Low-Latency C++20 Trading Bot Engine

A modular, high-performance **C++20 algorithmic trading engine** engineered for capital preservation and disciplined trickle compounding on a ring-fenced pilot account (**₹10,000 / ~\$115**).

Built for **Indian Equities (NSE/BSE)** with ₹0.05 tick-size precision, **Dual-Mode Parallel Paper Trading (`--mode both`)**, real-time **Live-Tail streaming (`--live-tail`)**, and automated cloud execution via **GitHub Actions + AngelOne SmartAPI + Google Sheets**, grounded in empirical market microstructure research.

---

## 🏛️ System Architecture

```text
[ Market Feed (CSV / Live-Tail / Broker API) ]  [ External Trader Webhook / Alert ]
                │                                           │
                ▼                                           ▼
[ Lock-Free SPSC Tick Ring Buffer ]            [ Lock-Free SPSC Alert Ring Buffer ]
                │                                           │
                ▼                                           ▼
[ L2 Order Book & Microstructure ] ──────────► [ Signal Ingestor ("BS Detector") ]
   ├── Multi-Level Depth OFI (Cont 2014)          ├── Local OFI >= 0.20 Check
   ├── Multi-Horizon Blend (Kolm 2023)            └── >2% Price / >4x Vol Trap Veto
   ├── Top-5 Bid/Ask Depth Ratio                            │
   └── Weighted Microprice & Spread                         │
                │                                           │
                ├───────────────────────────────────────────┘
                ▼
[ Dual-Mode Parallel Strategy & Risk Engine (--mode both) ]
   ├── Track A: High-Yield Compounder (Dynamic Daily Equity Compounding,
   │            Morning Institutional Focus, Tight -0.60% SL / +1.20% TP,
   │            Composite OFI >= 0.50 & Top-5 Bid/Ask Depth >= 1.35x)
   └── Track B: L2 Microstructure Harvester (Progressive Multi-Stage Ratchet,
                OFI Exhaustion Profit Harvest, Full Morning + Afternoon Windows)
                │
                ▼
[ Pre-Trade Risk Manager (13 Institutional Shields per Track) ]
   ├── Dynamic Fixed-Risk Sizing (<= 1.2% risk & <= 50% notional cap)
   ├── Net / Peak-to-Trough Daily Drawdown Kill-Switch (₹500 / 5%)
   ├── Zero-Loss Break-Even & Multi-Stage Trailing Stop
   ├── NSE Auto-Squareoff Shield (15:05 IST Cutoff)
   ├── Circuit Band Guard (1.5% distance check)
   ├── Macro Event Freeze (RBI / Budget / FOMC Calendar)
   └── Statutory Friction Filter (Expected Gain >= 2.5x Fees)
                │
                ▼
[ Paper Broker / Execution Gateway & Live Cloud Telemetry ]
   ├── Strict Zero Look-Ahead Bias (Tick[t] fills on Tick[t+1])
   ├── Almgren-Chriss Non-Linear Adverse Slippage Model
   ├── Penny-Accurate NSE Statutory Charges (STT, Turnover, GST, Stamp, SEBI)
   ├── Real-Time [LIVE_EVENT] JSON Stream ──► Google Sheets Live Dashboard
   └── 30-Day Session-by-Session & Head-to-Head Executive Audit Report
```

---

## ⚡ Dual-Mode Parallel Execution (`--mode both`) & 30-Day Benchmark

Every run of `deltauno` executes **two independent strategy/risk pipelines simultaneously in lockstep** on the exact same incoming L2 tick stream (or individual modes via `--mode compounder` / `--mode harvester`):

| Strategy Track | Core Mechanics | 30-Day Ending Equity (`₹10,000` Start) | Net Return | Completed Trades | Win Rate | Profit Factor | Max Intraday Drawdown | Kill-Switch Trips |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Track A: High-Yield Compounder** | Dynamic daily equity compounding (`1.2%` risk / `50%` notional scaled to current session equity), morning window (`09:30–11:00 IST`), tight `-0.60%` SL / `+1.20%` TP, `OFI >= 0.50`, `Bid/Ask Depth >= 1.35x` | **₹12,239.89** | **+22.40%** | 24 (21W / 3L) | **87.50%** | **16.32** | ₹105.29 (0.88%) | **0 / 30** |
| **Track B: L2 Microstructure Harvester** | Fixed ₹10,000 base sizing, morning + afternoon windows (`09:30–11:00` & `13:30–14:45 IST`), 3-stage progressive stop ratchet + `OFI_EXHAUSTION_PROFIT_HARVEST` when L2 buying pressure flips negative in profit | **₹10,607.79** | **+6.08%** | 61 (48W / 13L) | **78.69%** | **2.72** | ₹125.53 (1.20%) | **0 / 30** |

---

## ☁️ Automated Live NSE Paper Trading (GitHub Actions + Google Sheets)

DeltaUno includes a zero-cost, cloud-automated **Live Paper-Shadow Pipeline** that runs every weekday (`Mon–Fri`) during NSE market hours (`09:15–15:30 IST`):

1. **Scheduled GitHub Actions Runner** ([`.github/workflows/live_paper_trading.yml`](file:///d:/personal-proj/DeltaUno/.github/workflows/live_paper_trading.yml)):
   - Wakes up automatically at **`09:12 IST` (`03:42 UTC`)** (or on-demand via `workflow_dispatch`), compiles the C++20 engine with `-O3`, and launches [`scripts/run_live_paper_trading.py`](file:///d:/personal-proj/DeltaUno/scripts/run_live_paper_trading.py).
2. **Multi-Broker Live L2 Feed Orchestrator** ([`scripts/run_live_paper_trading.py`](file:///d:/personal-proj/DeltaUno/scripts/run_live_paper_trading.py)):
   - Authenticates with **AngelOne SmartAPI** (using built-in RFC 6238 TOTP generation with zero external dependencies), **Zerodha Kite Connect**, **Upstox v2**, or a **Zero-Config Yahoo/NSE fallback**.
   - Polls live NSE quotes and 5-level L2 order book depth across all 5 whitelisted symbols (`TATASTEEL`, `SBIN`, `ITC`, `COALINDIA`, `ZOMATO`) and appends them to `data/live/nse_live_YYYY-MM-DD.csv`.
3. **C++20 `--live-tail` Streaming Mode** ([`include/deltauno/market/csv_feed.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/market/csv_feed.hpp) & [`src/main.cpp`](file:///d:/personal-proj/DeltaUno/src/main.cpp)):
   - Tails the growing CSV file in real time with `std::streampos` mid-line rewind protection, processes every tick through both **Track A** and **Track B**, and emits structured `[LIVE_EVENT] {...}` JSON payloads on every `BUY`/`SELL` fill.
4. **Live Google Sheets Telemetry Bridge** ([`scripts/google_sheets_live_bridge.gs`](file:///d:/personal-proj/DeltaUno/scripts/google_sheets_live_bridge.gs)):
   - Pushes real-time `TRADE_FILL` events, 60-second `HEARTBEAT` status updates, and end-of-day `DAILY_SUMMARY` metrics to a Google Apps Script Web App maintaining three live tabs: **`Live_Dashboard`**, **`Live_Trades`**, and **`Daily_Summary`**.
   - All broker credentials and webhook URLs are stored exclusively in encrypted **GitHub Actions Repository Secrets** (`ANGELONE_API_KEY`, `ANGELONE_CLIENT_CODE`, `ANGELONE_PIN`, `ANGELONE_TOTP_SECRET`, `ANGELONE_PUBLIC_IP`, `GOOGLE_SHEETS_WEBHOOK_URL`).

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
   $$\text{Quantity} = \min\left(\left\lfloor \frac{\text{RiskCap}}{\text{Entry} - \text{SL}} \right\rfloor, \left\lfloor \frac{\text{NotionalCap}}{\text{Entry}} \right\rfloor\right)$$
   *(Base `₹120.00` risk / `₹5,000.00` notional in Track B; dynamically compounded at `1.2%` / `50%` of current session equity in Track A).*
2. **Daily Drawdown Kill-Switch**: Hard cutoff disables new entries if intraday net or peak-to-trough drawdown reaches **₹500.00 (5%)**, with automatic reset on IST day rollover.
3. **Zero-Loss Break-Even & Progressive Multi-Stage Trailing**: Ratchets Stop Loss to `ceil_to_tick(Entry + 0.15%, ₹0.05)` so all statutory taxes are covered, with progressive 3-stage profit locking and L2 OFI exhaustion harvesting in Track B.
4. **NSE Auto-Squareoff Shield**: Forcibly exits open intraday positions at **15:05 IST**, avoiding ₹50 + GST broker auto-squareoff penalties.
5. **Circuit Band Guard**: Vetoes entries within **1.5%** of Upper or Lower Circuit limits in both strategy and `RiskManager`.
6. **Time-of-Day Filter**: Restricts entries to high-liquidity institutional windows (**09:30–11:00** and **13:30–14:45 IST**).
7. **Zero Look-Ahead Bias**: Orders submitted on $\text{Tick}[t]$ match strictly on $\text{Tick}[t+1]$ (`timestamp_ns > created_time_ns`) at opposite book prices, guarded by an explicit `FLAT → PENDING_ENTRY → OPEN → PENDING_EXIT` state machine.
8. **Atomic Crash Recovery Journal**: Writes open position state via `.tmp` + atomic rename (`deltauno_state.json`) and restores `Position`, `RiskManager`, `PaperBroker`, and `Watchdog` on startup.
9. **Lock-Free Async Logger**: Bounded MPMC ring-buffer logger formats timestamps off the hot thread using C++20 atomic wait/notify (`deltauno.log`).
10. **Dual-Mode Network Watchdog**: Monitors live `steady_clock` stalls and deterministic replay tick-timestamp gaps ($> 5\text{s}$) whenever a position is open.
11. **Macro Event Freeze**: Precomputes UTC nanosecond windows from `config/calendar_events.json` to block entries during RBI, Union Budget, or FOMC announcements in $O(1)$ integer comparisons.
12. **Statutory Tax Pre-Check**: Pre-calculates STT (0.025%), NSE turnover (0.00297%–0.00345%), GST (18%), stamp duty (0.003%), and SEBI turnover fees (0.0001%) prior to order submission.
13. **Post-Trade Diagnostic Auditor**: Tracks tick-by-tick MAE (Maximum Adverse Excursion), MFE (Maximum Favorable Excursion), Win Rate, Profit Factor, exact Friction Drag Ratio, and 30-day dual-track session comparisons.

---

## 📂 Project Structure

```text
DeltaUno/
├── CMakeLists.txt                       # C++20 build & 9-suite CTest configuration (incl. 184-test E2E suite)
├── README.md                            # Master architecture & usage guide
├── TEST_INFRA.md                        # 4-Tier 184-test E2E verification specification
├── TEST_READY.md                        # E2E test sign-off & compliance report
├── .github/
│   └── workflows/
│       └── live_paper_trading.yml       # Automated Mon-Fri 09:12 IST cloud live paper trading workflow
├── config/
│   ├── risk_limits.json                 # ₹10,000 capital limits, windows & strategy parameters
│   ├── instruments.json                 # Whitelist of 5 liquid NSE stocks (₹0.05 tick)
│   ├── calendar_events.json             # Macro event freeze schedule (RBI, Budget, FOMC)
│   └── live_broker.env.example          # Template for broker & Google Sheets environment variables
├── docs/
│   ├── financial_governance_approval.md # Executive risk & capital sign-off charter
│   ├── implementation_plan.md           # Detailed system design & verification spec
│   └── walkthrough.md                   # Build, CTest, 30-day dual-mode & live cloud verification report
├── include/
│   └── deltauno/
│       ├── core/
│       │   ├── types.hpp                # Fixed-point Price, 5p tick helpers, POD structs
│       │   ├── ring_buffer.hpp          # Cache-aligned lock-free SPSC & MPMC queues
│       │   ├── async_logger.hpp         # Lock-free MPMC async logger (off-thread formatting)
│       │   ├── state_journal.hpp        # Atomic .tmp + rename crash recovery journal
│       │   └── config_loader.hpp        # Runtime JSON loader & IST civil-time converter
│       ├── market/
│       │   ├── order_book.hpp           # 5-level L2 book, multi-level OFI, depth ratio & microprice
│       │   ├── feed_interface.hpp       # Market feed & external alert callback interfaces
│       │   ├── csv_feed.hpp             # Deterministic replay & real-time --live-tail streaming feed
│       │   └── watchdog.hpp             # Dual-mode steady-clock & tick-gap watchdog
│       ├── strategy/
│       │   ├── strategy_interface.hpp
│       │   ├── momentum_breakout.hpp    # Dual-Mode (COMPOUNDER & HARVESTER) breakout engine
│       │   └── signal_ingestor.hpp      # JSON webhook parser & L2 "BS Detector"
│       ├── risk/
│       │   └── risk_manager.hpp         # All 13 pre-trade & open-position risk shields
│       └── execution/
│           ├── execution_interface.hpp
│           ├── paper_broker.hpp         # Tick[t+1] matching, slippage, taxes & MAE/MFE
│           └── trade_auditor.hpp        # Exact PnL identity & dual-mode 30-day comparison auditor
├── src/
│   └── main.cpp                         # Dual-Mode lock-free SPSC pipeline & [LIVE_EVENT] streamer
├── data/
│   ├── historical_30days_nse.csv        # 30-session (568,819-tick) realistic NSE L2 dataset
│   ├── sample_nse_ticks.csv             # Deterministic single-session NSE momentum breakout L2 stream
│   └── chop_ticks.csv                   # Sideways rangebound noise dataset
├── scripts/
│   ├── run_live_paper_trading.py        # Multi-broker (AngelOne/Zerodha/Upstox/Yahoo) live orchestrator
│   ├── google_sheets_live_bridge.gs     # Google Apps Script webhook for live Google Sheets dashboard
│   ├── generate_30day_data.py           # 30-session multi-symbol NSE L2 generator
│   └── generate_nse_ticks.py            # Seeded NSE ₹0.05-tick scenario generator
└── tests/
    ├── test_macros.hpp                  # Release-safe (-DNDEBUG) test assertion macros
    ├── test_order_book.cpp              # L2 depth, OFI, circuit & SPSC concurrency tests
    ├── test_signal_ingestor.cpp         # Webhook JSON & BS Detector veto unit tests
    ├── test_risk_manager.cpp            # Sizing, drawdown, rollover, windows & macro tests
    ├── test_state_journal.cpp           # Atomic WAL crash recovery & Watchdog tests
    ├── test_paper_broker.cpp            # Tick[t+1], slippage, taxes & MAE/MFE tests
    ├── test_trade_auditor.cpp           # Exact PnL identity & scratch-trade audit tests
    └── e2e/
        └── run_e2e_tests.py             # 184-test 4-Tier End-to-End verification suite
```

---

## 🚀 Building & Running

### Prerequisites
- CMake 3.20+
- C++20 compliant compiler (Clang / GCC MinGW / MSVC)
- Python 3.9+ (standard library only; zero `pip` packages required)

### Build All Targets
```powershell
cmake -G "MinGW Makefiles" -B build
cmake --build build -j 4
```

### Run Automated Test Suites (9/9 CTest Suites, 184 E2E Tests)
```powershell
ctest --test-dir build --output-on-failure
```

### Run 30-Day Dual-Mode Parallel Simulation (`--mode both`)
```powershell
# Default runs both Track A (Compounder) and Track B (Harvester) in parallel
.\build\deltauno.exe data/historical_30days_nse.csv

# Run an individual strategy track
.\build\deltauno.exe data/historical_30days_nse.csv --mode compounder
.\build\deltauno.exe data/historical_30days_nse.csv --mode harvester

# Choppy sideways market (verifies bot stays flat with 0 fee bleed)
.\build\deltauno.exe data/chop_ticks.csv

# Test external trader webhook validation through the local L2 "BS Detector"
.\build\deltauno.exe data/chop_ticks.csv --webhook "{\"symbol\":\"PENNYPUMP\",\"side\":\"BUY\",\"entry\":12.00,\"sl\":11.50}"
```

### Run Live NSE Paper Trading Orchestrator
```powershell
# 1. Verify live-tail streaming & dual-mode execution end-to-end (offline self-test)
python scripts/run_live_paper_trading.py --self-test

# 2. Verify live broker API authentication & 5-symbol NSE L2 quote retrieval
python scripts/run_live_paper_trading.py --broker angelone --test-connection

# 3. Verify live Google Sheets webhook bridge
python scripts/run_live_paper_trading.py --test-sheets

# 4. Run full NSE live paper trading session (09:15 - 15:30 IST)
python scripts/run_live_paper_trading.py --broker angelone --mode both
```
