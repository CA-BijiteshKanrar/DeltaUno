# TradeUno: Low-Latency C++ Trading Bot Engine

## Goal Description
Build **TradeUno**, a modular, high-performance C++20 algorithmic trading bot engine engineered for capital preservation and disciplined trickle growth on a micro-account of **₹1,000 (~$10)**.

The system is designed for **Indian Equities (NSE/BSE)** with multi-asset extensibility, providing:
1. **Paper Trading Simulation Engine** with realistic statutory friction (STT, GST, exchange turnover, stamp duty, slippage) and zero look-ahead bias matching.
2. **Hybrid Signal Pipeline**: An internal volume breakout & Order Flow Imbalance (OFI) detector combined with a Webhook/Signal Ingestion endpoint to mirror external trader alerts with local validation ("BS Detector").
3. **13 Institutional-Grade Risk & Operational Safeguards**:
   - Zero-Loss Break-Even Trailing Stop
   - NSE Auto-Squareoff Shield (15:05 IST auto-exit avoiding broker penalty fees)
   - "BS Detector" (Order book verification of external trader signals)
   - Dynamic Fixed-Risk Sizing ($Risk \le \text{₹}12$ per trade)
   - Circuit Band Guard (avoiding 0-liquidity traps near Upper/Lower Circuits)
   - Crash Recovery State Journal (`state.json` Write-Ahead Logging)
   - Network Heartbeat Watchdog
   - Zero-Jitter Lock-Free Async Logger
   - 100% Backtest-to-Live Code Parity
   - Zero Look-Ahead Bias Clocking ($Tick[t]$ signals fill on $Tick[t+1]$)
   - Post-Trade Diagnostic Auditor (MAE, MFE, Friction Drag Ratio)
   - Macro Event / RBI Volatility Freeze
   - Strict Separation of Concerns (Strategy $\to$ Risk Engine $\to$ Gateway)
4. **Execution Gateway**: Abstracted interface starting with the simulated paper broker and ready for zero-brokerage live execution (Shoonya/Finvasia).

---

## User Review Required

> [!IMPORTANT]
> **Toolchain Installation**: The Windows environment currently lacks `cmake` and a C++ compiler. As step 1, we will install `cmake` and `mingw` (GCC) via Chocolatey / Winget.

> [!NOTE]
> **Zero-Brokerage Mandate**: Because standard brokers charge ₹20 per trade (4% of a ₹1,000 account round-trip), live trading is targeted strictly for **zero-brokerage brokers** (such as Shoonya/Finvasia). All initial validation runs in our high-fidelity paper engine where statutory taxes and slippage are accurately deducted.

---

## Architecture Overview

```mermaid
flowchart TD
    subgraph MarketData [Data & Signal Ingestion Layer]
        CSV[CSV Historical Tick Replayer] --> FeedRouter[Feed Multiplexer]
        WS[Live WebSocket Feed] --> FeedRouter
        WH[External Trader Alert Webhook] --> WebhookParser[HTTP / JSON Signal Ingestor]
    end

    subgraph CoreEngine [Core Trading Engine (C++20)]
        FeedRouter --> SPSC[Lock-Free SPSC Ring Buffer]
        WebhookParser --> SPSC
        SPSC --> OB[L2 Order Book & Depth Imbalance Engine]
        OB --> BSFilter["BS Detector" Signal Validation]
        BSFilter --> Strategy[Momentum Breakout & Signal Engine]
        
        subgraph RiskShield [Pre-Trade Risk & Operational Guard]
            Strategy --> MacroGate[Macro Event & RBI Calendar Freeze]
            MacroGate --> TimeGate[Time-of-Day Filter & 15:05 Cutoff]
            TimeGate --> CircuitGuard[Circuit Band Liquidity Guard]
            CircuitGuard --> Sizer[Dynamic Fixed-Risk Sizer <= ₹12]
            Sizer --> FeeFilter[Fee-to-Payoff Filter > 2.5x]
            FeeFilter --> TrailingStop[Zero-Loss Break-Even Trailing Stop]
        end
    end

    subgraph StorageAndTelemetry [Zero-Jitter Storage & Telemetry]
        RiskShield --> WAL[Write-Ahead State Journal / Crash Recovery]
        CoreEngine -.-> AsyncLog[Lock-Free Async Ring-Buffer Logger]
        Execution --> Auditor[Post-Trade Auditor: MAE, MFE, Friction Ratio]
    end

    subgraph Execution [Execution Gateway]
        RiskShield --> Gateway[Execution Gateway Interface]
        Gateway --> PaperBroker[Paper Trading Engine\n(Tick[t+1] Matching, Slippage & Taxes)]
        Gateway -.-> LiveBroker[Shoonya Zero-Brokerage API]
    end

    PaperBroker --> Account[Account Tracker & PnL Ledger]
    Account -.-> RiskShield
```

---

## Detailed Directory & Component Structure

```
tradeUno/
├── CMakeLists.txt
├── config/
│   ├── risk_limits.json           # ₹1,000 capital limits, max loss, cutoff times
│   ├── instruments.json           # Whitelist of liquid NSE stocks under ₹1,000
│   └── calendar_events.json       # Macro event freeze schedule (RBI, budget, etc.)
├── include/
│   └── tradeuno/
│       ├── core/
│       │   ├── types.hpp          # Price, Qty, Tick, Order, AssetClass, Side
│       │   ├── ring_buffer.hpp    # Cache-aligned lock-free SPSC queue
│       │   ├── async_logger.hpp   # Non-blocking async logger
│       │   └── state_journal.hpp  # Write-Ahead crash recovery journal
│       ├── market/
│       │   ├── order_book.hpp     # Fast L2 depth book & OFI calculation
│       │   ├── feed_interface.hpp # Base feed interface for backtest & live
│       │   ├── csv_feed.hpp       # Deterministic tick replayer
│       │   └── watchdog.hpp       # Network heartbeat watchdog timer
│       ├── strategy/
│       │   ├── strategy_interface.hpp
│       │   ├── momentum_breakout.hpp # Volume acceleration & micro-breakout
│       │   └── signal_ingestor.hpp   # Webhook/alert receiver with "BS detector"
│       ├── risk/
│       │   └── risk_manager.hpp   # All 13 risk guards, friction filter, trailing SL
│       └── execution/
│           ├── execution_interface.hpp
│           ├── paper_broker.hpp   # Matching engine with STT/GST/Slippage
│           └── trade_auditor.hpp  # MAE, MFE, Friction Drag Ratio calculator
├── src/
│   ├── main.cpp                   # Engine entry point & orchestration
│   ├── core/
│   │   ├── async_logger.cpp
│   │   └── state_journal.cpp
│   ├── market/
│   │   ├── order_book.cpp
│   │   └── csv_feed.cpp
│   ├── strategy/
│   │   ├── momentum_breakout.cpp
│   │   └── signal_ingestor.cpp
│   ├── risk/
│   │   └── risk_manager.cpp
│   └── execution/
│       ├── paper_broker.cpp
│       └── trade_auditor.cpp
├── data/
│   └── sample_nse_ticks.csv       # Sample high-frequency NSE market depth data
└── tests/
    ├── test_order_book.cpp
    ├── test_risk_manager.cpp
    ├── test_state_journal.cpp
    ├── test_trade_auditor.cpp
    └── test_paper_broker.cpp
```

---

## Component Details

### 1. Build System & Toolchain Setup

#### [NEW] `CMakeLists.txt`
Configures modern C++20 build target, strict compiler warnings (`-Wall -Wextra -Wpedantic`), thread support (`pthread`), and optimization flags (`-O3 -march=native`).

---

### 2. Core Architecture & Types (`include/tradeuno/core/`)

#### [NEW] `include/tradeuno/core/types.hpp`
Defines zero-heap-allocation, cache-aligned core types:
- `Price`: Fixed-point integer in paise (1 INR = 100 paise) to eliminate IEEE-754 float precision errors.
- `Quantity`: Unsigned integer shares.
- `AssetClass`: `EQUITY_CASH`, `EQUITY_INTRADAY_MARGIN`, `EQUITY_FUTURES`.
- `MarketTick`: Timestamp, symbol token, LTP, total volume, top 5 bid/ask levels (price & quantity), upper/lower circuit limits.
- `Order`: Client ID, symbol, side, order type, price, qty, status, filled qty, avg price.
- `TradeAuditEntry`: Order IDs, fill timestamp, fill price, exit price, MAE (Max Adverse Excursion), MFE (Max Favorable Excursion), statutory taxes breakdown, gross PnL, net PnL.

#### [NEW] `include/tradeuno/core/ring_buffer.hpp`
Single-Producer Single-Consumer (SPSC) lock-free ring buffer for passing ticks and external signals with sub-microsecond latency.

#### [NEW] `include/tradeuno/core/async_logger.hpp`
Background consumer thread writing log entries to disk and stdout without blocking the trading hot path.

#### [NEW] `include/tradeuno/core/state_journal.hpp`
Write-Ahead Logging (WAL) state manager that flushes open positions and active orders to `state.json`. On engine startup, it recovers active trades to prevent orphaned positions after a crash.

---

### 3. Market Depth & Data Feed (`include/tradeuno/market/`)

#### [NEW] `include/tradeuno/market/order_book.hpp`
Fast, static-array based Level 2 order book:
- Tracks top 5 Bid & Ask depth levels.
- Calculates weighted mid-price and bid-ask spread.
- Computes Order Flow Imbalance (OFI):
  $$\text{OFI} = \Delta \text{BidSize} - \Delta \text{AskSize}$$

#### [NEW] `include/tradeuno/market/csv_feed.hpp`
High-speed tick replayer reading timestamped CSV files with full depth.

#### [NEW] `include/tradeuno/market/watchdog.hpp`
Monitors time elapsed since the last tick received. If > 5 seconds during active trading hours with an open position, triggers an emergency alert.

---

### 4. Strategy & Signal Pipeline (`include/tradeuno/strategy/`)

#### [NEW] `include/tradeuno/strategy/momentum_breakout.hpp`
- Calculates 20-period moving average volume.
- Detects volume expansion $> 2.5\times$ moving average with price crossing above VWAP / local resistance.
- Whitelist check: Restricts trading to approved liquid NSE stocks under ₹1,000 (e.g. TATASTEEL, SBIN, ITC, COALINDIA, ZOMATO).

#### [NEW] `include/tradeuno/strategy/signal_ingestor.hpp` & "BS Detector"
- Receives external trader alerts (via JSON).
- Passes the alert through the **BS Detector**:
  1. Checks if current volume is elevated.
  2. Confirms Order Flow Imbalance (OFI) is positive.
  3. Rejects if price already surged $> 2\%$ in the last 60 seconds (prevents buying at the peak of a pump).

---

### 5. Risk Management Engine (`include/tradeuno/risk/`)

#### [NEW] `include/tradeuno/risk/risk_manager.hpp`
Enforces the complete suite of capital protection rules:
- **Capital Bound**: Account capital initialized at ₹1,000.
- **Dynamic Fixed-Risk Sizing**:
  $$\text{Quantity} = \min\left(\left\lfloor \frac{\text{₹}12}{\text{Entry} - \text{SL}} \right\rfloor, \left\lfloor \frac{\text{₹}500}{\text{Entry}} \right\rfloor\right)$$
- **Zero-Loss Break-Even Trailing**: When profit $\ge +1\%$, SL is moved to $\text{Entry} + \text{Estimated Taxes}$.
- **Daily Drawdown Kill-Switch**: Halts all new orders if daily drawdown reaches **₹50 (5%)**.
- **Time-of-Day Filter**: Allows entries only during **09:30–11:00** and **13:30–14:45 IST**.
- **Auto-Squareoff Shield**: Forcibly exits all open intraday positions at **15:05 IST** (avoiding the broker's ₹50 penalty).
- **Circuit Band Guard**: Vetoes orders if price is within 1.5% of Upper or Lower Circuit.
- **Macro Event Freeze**: Rejects entries during scheduled high-impact event windows.
- **Statutory Friction Filter**: Calculates estimated STT, GST, turnover, stamp duty. Rejects any trade where expected profit $< 2.5\times$ fees.

---

### 6. Execution & Paper Broker (`include/tradeuno/execution/`)

#### [NEW] `include/tradeuno/execution/paper_broker.hpp`
High-fidelity simulated exchange matching engine:
- **Zero Look-Ahead Bias**: Orders placed on $Tick[t]$ fill on $Tick[t+1]$ at opposite best bid/ask.
- Simulates realistic 10ms execution latency and slippage.
- Accurately computes statutory charges:
  - STT: 0.025% on intraday sell value.
  - Exchange Turnover Fee: 0.00345% of turnover.
  - GST: 18% on (brokerage + turnover fee).
  - Stamp Duty: 0.003% on buy value.
  - SEBI Charges: ₹10 per crore of turnover.

#### [NEW] `include/tradeuno/execution/trade_auditor.hpp`
Post-trade diagnostic evaluator:
- Computes **MAE (Maximum Adverse Excursion)** and **MFE (Maximum Favorable Excursion)** per trade.
- Calculates **Friction Drag Ratio**:
  $$\text{Friction Ratio} = \frac{\text{Total Taxes \& Charges}}{\text{Gross Profit}}$$
- Computes Expectancy, Win Rate, and Profit Factor.

---

### 7. Engine Entry Point

#### [NEW] `src/main.cpp`
Orchestrates:
1. State recovery check via `StateJournal`.
2. Initializing `RiskManager` with ₹1,000 capital.
3. Loading liquid instrument whitelist and event calendar.
4. Starting background async logger.
5. Starting market feed replayer and event loop.
6. Printing end-of-run performance audit report (Win Rate, Profit Factor, Gross PnL, Total Taxes & Friction, Net PnL, Max Drawdown, MAE/MFE statistics).

---

## Verification Plan

### Automated Tests
1. **Risk Engine Invariant Tests** (`tests/test_risk_manager.cpp`):
   - Test that single trade loss never exceeds ₹12.
   - Test that daily drawdown kill-switch engages at -₹50.
   - Test that trades between 11:30–13:30 IST and after 15:00 IST are rejected.
   - Test that circuit band guard rejects trades near price band limits.
   - Test that break-even trailing stop activates at +1% profit.
   - Test macro event freeze blocks trades during event windows.
2. **Order Book & BS Detector Tests** (`tests/test_order_book.cpp`):
   - Test depth parsing, OFI calculation, and pump-and-dump signal veto.
3. **Paper Broker & Tax Tests** (`tests/test_paper_broker.cpp`):
   - Test exact penny-accurate calculation of STT, GST, turnover, stamp duty.
   - Verify tick $t+1$ execution order without look-ahead bias.
4. **Crash Recovery Tests** (`tests/test_state_journal.cpp`):
   - Simulate a crash with an open position, restart engine, verify state recovery.
5. **Trade Auditor Tests** (`tests/test_trade_auditor.cpp`):
   - Validate MAE, MFE, and friction drag ratio calculations.

### Manual Verification
1. Run `tradeuno` in backtest mode using `data/sample_nse_ticks.csv`.
2. Inspect the generated terminal and file log:
   - Verify all trades comply with risk sizing.
   - Verify that 15:05 auto-squareoff successfully closed positions.
   - Verify final net balance, fee breakdown, and trade ledger.
