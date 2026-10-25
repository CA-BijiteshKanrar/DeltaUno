# DeltaUno Implementation & Verification Walkthrough

## Summary of Accomplishments

We engineered, audited, and verified **DeltaUno**: a modular, low-latency **C++20 algorithmic trading engine** built for disciplined capital preservation on a **₹10,000 (~\$115)** pilot allocation on **Indian Equities (NSE/BSE)** with ₹0.05 tick-size precision and full backtest-to-live architectural parity.

---

## 1. Toolchain & Build Configuration
- **Compiler**: `MartinStorsjo.LLVM-MinGW.UCRT` (Clang 23.1.4, LLD, UCRT runtime) targeting `-std=c++20` with `-Wall -Wextra -Wpedantic -O3` (zero warnings across all targets).
- **Build System**: [CMakeLists.txt](file:///d:/personal-proj/DeltaUno/CMakeLists.txt) defining the `deltauno_engine` header-only `INTERFACE` target linked against `Threads::Threads`, the `deltauno` executable, and 6 Release-safe CTest suites.

---

## 2. Core Components Verified

### Core Infrastructure (`include/deltauno/core/`)
- [`types.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/core/types.hpp): Fixed-point `Price` in paise (`int64_t`), NSE ₹0.05 tick alignment (`round_to_tick`, `ceil_to_tick`, `floor_to_tick`), and `trivially_copyable` hot-path structures (`MarketTick`, `ExternalAlert`, `Order`, `Position`).
- [`ring_buffer.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/core/ring_buffer.hpp): Cache-line aligned (`64B`) lock-free `SPSCRingBuffer` for producer-consumer market/webhook ingestion and bounded Vyukov `MPMCRingBuffer` for multi-producer logging.
- [`async_logger.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/core/async_logger.hpp): Lock-free MPMC ring-buffer logger using C++20 `std::atomic::wait` / `notify_one`, formatting timestamps off the trading thread (`deltauno.log`).
- [`state_journal.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/core/state_journal.hpp): Atomic `.tmp` + rename write-ahead state persistence (`deltauno_state.json`), wired into [main.cpp](file:///d:/personal-proj/DeltaUno/src/main.cpp) and [`PaperBroker`](file:///d:/personal-proj/DeltaUno/include/deltauno/execution/paper_broker.hpp) for seamless crash recovery.
- [`config_loader.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/core/config_loader.hpp): Runtime JSON loader for [risk_limits.json](file:///d:/personal-proj/DeltaUno/config/risk_limits.json), [instruments.json](file:///d:/personal-proj/DeltaUno/config/instruments.json), and [calendar_events.json](file:///d:/personal-proj/DeltaUno/config/calendar_events.json) with precomputed UTC-ns macro freeze intervals.

### Market Microstructure, Strategy, Risk & Execution
- [`order_book.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/market/order_book.hpp): 5-level L2 depth book computing exponentially weighted multi-level OFI (*Cont et al. 2014*), multi-horizon decay (*Kolm et al. 2023*), weighted microprice, and 1.5% circuit proximity guards.
- [`watchdog.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/market/watchdog.hpp): Dual-mode network watchdog separating `steady_clock` live stall detection from deterministic replay tick-timestamp gap monitoring.
- [`momentum_breakout.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/strategy/momentum_breakout.hpp) & [`signal_ingestor.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/strategy/signal_ingestor.hpp): Internal volume-acceleration ($\ge 2.5\times$) + OFI ($\ge 0.55$) + VWAP strategy alongside external JSON webhook validation ("BS Detector" rejecting weak OFI, $>2\%$ price pumps, and $>4\times$ volume climaxes).
- [`risk_manager.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/risk/risk_manager.hpp): Enforces all 13 safeguards including ₹120 fixed-risk sizing, ₹5,000 notional cap, ₹500 net/peak-to-trough daily drawdown with IST day rollover reset, ₹0.05-aligned zero-loss break-even trailing stop, 15:05 IST auto-squareoff, macro calendar freeze, circuit guard, and 2.5x statutory fee filter.
- [`paper_broker.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/execution/paper_broker.hpp) & [`trade_auditor.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/execution/trade_auditor.hpp): Strict `Tick[t+1]` execution, `ceil`/`floor` Almgren-Chriss slippage, penny-accurate NSE statutory charges (STT, turnover, GST, stamp duty, SEBI), tick-by-tick MAE/MFE tracking, and exact PnL accounting across win, loss, and scratch trades.

---

## 3. Verification & Test Results

### Automated CTest Suite (6/6 Passing)
All 6 Release-safe unit test suites (`DU_EXPECT` assertions active even under `-O3 -DNDEBUG`) pass at 100%:

```text
Test project D:/personal-proj/DeltaUno/build
    Start 1: OrderBookTest
1/6 Test #1: OrderBookTest ....................   Passed    0.02 sec
    Start 2: SignalIngestorTest
2/6 Test #2: SignalIngestorTest ...............   Passed    0.17 sec
    Start 3: RiskManagerTest
3/6 Test #3: RiskManagerTest ..................   Passed    0.13 sec
    Start 4: StateJournalTest
4/6 Test #4: StateJournalTest .................   Passed    0.20 sec
    Start 5: PaperBrokerTest
5/6 Test #5: PaperBrokerTest ..................   Passed    0.16 sec
    Start 6: TradeAuditorTest
6/6 Test #6: TradeAuditorTest .................   Passed    0.02 sec

100% tests passed out of 6
Total Test time (real) = 0.92 sec
```

### Live Simulation Backtest — Momentum Breakout (`data/sample_nse_ticks.csv`)

```text
[INFO ] =================================================
[INFO ]    DeltaUno Low-Latency C++20 Trading Engine     
[INFO ]    Target: NSE/BSE Equities | Capital: INR 10000
[INFO ] =================================================
[INFO ] Risk Manager active: Capital=10000 INR | MaxRisk/Trade=120 INR | KillSwitch=500 INR | Whitelist=5 symbols | MacroEvents=3
[INFO ] State Journal clean. No orphaned positions found.
[INFO ] Connecting to CSV Market Feed: data/sample_nse_ticks.csv
[INFO ] Starting Market Feed & Lock-Free SPSC Pipeline...
[INFO ] SIGNAL ACCEPTED (MOMENTUM): TATASTEEL BUY Qty=33 @ Target=150.25 INR | OFI=0.60
[TRADE] FILLED BUY: TATASTEEL Qty=33 @ 150.42 INR | SL=148.85 | TP=153.15
[INFO ] BREAKEVEN RATCHETED: TATASTEEL SL moved to 150.65 INR (Zero-Loss locked)
[INFO ] Position Exit Triggered: TAKE_PROFIT_TRIGGERED @ 153.15 INR
[TRADE] FILLED SELL: TATASTEEL Qty=33 @ 153.23 INR | Gross PnL: +92.73 INR | Taxes: -1.83 INR | Net PnL: +90.90 INR | MAE: 0.00 INR | MFE: 2.83 INR | Balance: 10090.90 INR
[INFO ] Feed replay complete.

============================================================
              DELTAUNO PERFORMANCE AUDIT REPORT             
============================================================
  Total Completed Trades    : 1
  Winning Trades            : 1 (100.00%)
  Losing Trades             : 0
  Break-Even Trades         : 0
------------------------------------------------------------
  Gross Profit              : +92.73 INR
  Gross Loss                : -0.00 INR
  Total Statutory Friction  : -1.83 INR (STT/GST/Turnover/SEBI)
  Net PnL                   : +90.90 INR
  Profit Factor             : 999.00
  Expectancy / Trade        : +90.90 INR
  Friction Drag Ratio       : 1.97% of gross profit
------------------------------------------------------------
  Avg MAE (Adverse Move)    : 0.00 INR
  Avg MFE (Favorable Move)  : 2.83 INR
============================================================
```

### Negative & Safety Invariant Scenarios
- **Sideways Chop (`data/chop_ticks.csv`)**: **0 trades executed** — preserves the ₹10,000 capital from statutory fee bleed.
- **Upper Circuit Surge (`--scenario circuit`)**: **0 trades executed** — vetoed within 1.5% of upper circuit limit.
- **Unverified External Webhook (`--webhook '{"symbol":"PENNYPUMP",...}'`)**: **0 trades executed** — immediately vetoed by the local L2 BS Detector.
