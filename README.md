# DeltaUno: C++20 3-Track Intraday Paper-Trading & Microstructure Research Laboratory

A deterministic **C++20 intraday trading engine and forward-testing laboratory** built to evaluate order-flow imbalance (OFI) and momentum hypotheses on Indian Equities (**NSE/BSE**, ₹0.05 tick precision) using a ring-fenced **₹10,000 (~$115) per-track research capital baseline**.

DeltaUno runs **three parallel strategy tracks (`--mode both`: Track A Compounder, Track B Harvester, Track C Bi-Directional Omni)** on a shared 5-level Level-2 quote stream (`--live-tail`), with multi-broker REST polling (**Kotak Neo Trade API v2**, **AngelOne SmartAPI**, **Upstox API v2**) and automated logging to Google Sheets via GitHub Actions.

> [!IMPORTANT]
> **Research Status & Parameter Freeze (`v3.3.0`)**:
> - **Parameters Frozen as of `2026-11-02`**: Sessions from `2026-10-26` to `2026-10-30` (`5` sessions, `534,900` recorded ticks) were used to iterate on strategy rules, universe selection, and exit timing, and are strictly **in-sample (`REPLAY_V3.2`)**.
> - **Out-of-Sample (`LIVE_OOS`) Validation Window**: Starting **`2026-11-02` onward**, all strategy parameters, thresholds, position-sizing rules, and execution windows are frozen. No strategy edge is claimed until **60–100+ out-of-sample sessions (`>= 100` signals per track)** are completed under the pre-committed pass/fail criteria in [Section 2](#-out-of-sample-validation-protocol--passfail-criteria).

---

## 🏛️ System Architecture

```text
[ Live NSE 1s REST Poller (Kotak Neo / AngelOne ⇄ Upstox) ]     [ External Webhook / Alert ]
                          │ (~190–210 ms HTTP Poll Latency)                   │
                          ▼                                                   ▼
          [ Lock-Free SPSC Tick Ring Buffer ]                 [ Lock-Free SPSC Alert Ring Buffer ]
                          │                                                   │
                          ▼                                                   ▼
          [ 5-Level L2 Order Book & Microstructure ] ───────► [ Signal Ingestor (Heuristic Filter) ]
             ├── 5-Level Depth OFI (inspired by Cont 2014)       ├── Local OFI >= 0.20 Check
             ├── Multi-Horizon OFI Blend (1/5/20 ticks)          └── >2% Price / >4x Vol Spike Veto
             ├── Top-5 Bid/Ask & Ask/Bid Depth Ratios                         │
             ├── Top-of-Book L1 Liquidity Filter (Qty >= 50)                  │
             └── Queue-Weighted Microprice & Session VWAP                     │
                          │                                                   │
                          ├───────────────────────────────────────────────────┘
                          ▼
          [ 3-Track Parallel Strategy & Risk Engine (--mode both) ]
             ├── Track A: Compounder (Long-Only, 1.0x Cash, 1.20% Risk/Trade,
             │            09:35–11:00 & 13:30–14:15 IST, -0.85% SL / +1.75% TP, +0.90% BE Ratchet)
             ├── Track B: Harvester (Long-Only, 1.0x Cash, 1.20% Risk/Trade,
             │            09:35–11:00 & 13:30–14:45 IST, -0.55% SL, +0.55% Min Harvest Floor)
             └── Track C: Bi-Directional Omni (1 Long + 1 Short Slot, 2.15x MIS Leverage [1.80% Risk],
                          09:35–14:45 IST Window, reported both raw 2.15x and 1.0x Leverage-Normalized)
                          │
                          ▼
          [ Pre-Trade Risk Manager (14 Deterministic Risk Controls per Track) ]
             ├── Fixed-Risk Position Sizing (1.2%–1.8% risk & 1.0x–2.15x MIS notional cap)
             ├── NIFTY 50 Regime Filter (Blocks Longs when NIFTY drops <= -0.35% below Open)
             ├── Intraday Drawdown Kill-Switch (₹500 / 5% daily loss limit)
             ├── Break-Even Stop Ratchet (+0.90% Long / +0.45% Short trigger -> +0.15% lock)
             ├── Pre-RMS Intraday Auto-Squareoff (15:14:00 IST — 1 min before 15:15 broker RMS cutoff)
             ├── Circuit Band Guard (1.5% distance check) & Macro Event Freeze (RBI / Budget / FOMC)
             └── Statutory Friction Filter (Expected Gain >= 2.5x NSE MIS Statutory Taxes)
                          │
                          ▼
          [ Paper Broker & Provenance-Tagged Telemetry ]
             ├── Zero Look-Ahead Bias (Tick[t] orders match strictly on Tick[t+1])
             ├── Adverse Bid/Ask Sweep Slippage Model
             ├── NSE MIS Statutory Taxes + Explicit 0.03% Discount Broker vs. ₹0 Kotak Neo API Audit
             └── Provenance-Tagged Google Sheets Ledger (REPLAY_V3.2 vs. LIVE_OOS vs. REPLAY_EOD_FIX)
```

---

## 📊 Empirical Results, Provenance Split & Known Limitations

### 1. 6-Session NSE Recorded Tick Ledger (`2026-10-26` to `2026-11-02` — `714,972` Ticks)

To prevent conflating retroactive parameter tuning with live forward performance, every trade and session in the ledger is explicitly tagged by **Provenance**:
- **`REPLAY_V3.2 (IN-SAMPLE)` (`2026-10-26` to `2026-10-30`, 5 sessions)**: Recorded live from AngelOne/Upstox, but replayed after `v3.1`/`v3.2` parameter adjustments (`10`-stock universe expansion, `2.15x` MIS sizing on Track C, `-0.55%` SL on Track B, `15:14 IST` square-off). Treated strictly as **in-sample fitting data**.
- **`LIVE_OOS` (`2026-11-02` onward)**: Executed live in real time under frozen `v3.2` rules (`4` of `6` trades closed live before `15:05 IST`; the `2` open `HINDALCO` positions in Track A and Track C were squared off via `REPLAY_EOD_FIX` at `15:14:01 IST` due to the `350`-minute workflow timer bug fixed in `v3.3.0`).

| Strategy Track | Leverage & Sizing | In-Sample Replay (`Oct 26–30`, 5d) | Out-of-Sample (`Nov 02`, 1d) | 6-Day Ending Equity | Net PnL (**Kotak Neo `₹0` Brk**) | Net PnL (`0.03%` Brk) | `1.0x` Norm Return | Avg Win / Avg Loss | Net Expectancy (`₹/trd`) | Win Rate (`40` Trades) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Track C: Bi-Directional Omni (`OMNI`)** | `2.15x` MIS (`1.80%` Risk) | `+₹739.09` (`11T`) | `+₹156.57` (`2T`) | `₹10,895.66` | **`+₹895.66 (+8.96%)`** | `+₹781.52 (+7.82%)` | **`+4.17%`** | `+₹105.51` / `-₹132.46` | **`+₹68.90/trd`** | `84.6%` (`11W/2L`) |
| **Track A: Compounder** | `1.0x` Cash (`1.20%` Risk) | `+₹88.40` (`10T`) | `+₹113.98` (`2T`) | `₹10,202.38` | **`+₹202.38 (+2.02%)`** | `+₹128.10 (+1.28%)` | **`+2.02%`** | `+₹37.61` / `-₹45.38` | **`+₹16.86/trd`** | `75.0%` (`9W/3L`) |
| **Track B: Harvester** | `1.0x` Cash (`1.20%` Risk) | `+₹68.60` (`13T`) | `+₹69.52` (`2T`) | `₹10,138.12` | **`+₹138.12 (+1.38%)`** | `+₹45.27 (+0.45%)` | **`+1.38%`** | `+₹32.50` / `-₹54.85` | **`+₹9.21/trd`** | `73.3%` (`11W/4L`) |
| **Combined (`₹30,000` Start)** | All 3 Tracks | `+₹896.09` (`34T`) | `+₹340.07` (`6T`) | `₹31,236.16` | **`+₹1,236.16 (+4.12%)`** | `+₹954.89 (+3.18%)` | **`+2.52%`** | — | **`+₹30.90/trd`** | `77.5%` (`31W/9L`) |

### 2. Market Baseline Comparison (`Oct 26` – `Nov 02`)
During this 6-session window, the underlying 10-stock NSE universe experienced a broad bullish tailwind:
- **Equal-Weighted Buy-and-Hold across Active Traded Universe** (`TATASTEEL +5.1%`, `TRENT +6.4%`, `ZOMATO +5.6%`, `HINDALCO +3.0%`, `BEL +2.0%`, `SBIN +2.3%`, `ITC -0.9%`, `COALINDIA -1.6%` from Oct 26 open to Nov 02 close): **`~+2.74%` unlevered (`1.0x`)**.
- **Implication**: Because Tracks A (`+2.02%`) and B (`+1.38%`) are long-only and carry ~40–50% time-in-market cash exposure, their positive 6-day return cannot yet be separated from the rising-market beta. Only a multi-month out-of-sample run across flat and declining NSE regimes (and comparison against a random-entry / matched-exit baseline) can isolate true microstructure alpha.

### 3. Honest Assessment of Strategy Shape, Architecture & Costs
1. **Leverage vs. Signal Quality (`Track C`)**: Track C's headline `+8.96%` return is driven roughly `2.15x` by MIS intraday leverage (`1.80%` risk per slot). Scaled to the same `1.0x` cash basis as Tracks A and B, Track C returned **`+4.17%`**, and roughly `86%` of its gross profit came from four large trend captures (`ITC` Short `+₹221.30`, `ZOMATO` Long `+₹227.44`, `ITC` Long `+₹321.58`, `HINDALCO` Long `+₹131.51`).
2. **Negative Win/Loss Payoff Ratio in `Track B`**: While Track B (`HARVESTER`) has a `73.3%` win rate (`11W/4L`), its **average win (`+₹32.50`) is smaller than its average loss (`-₹54.85`, R:R `0.59`)** because early profit harvesting (`+0.55%` floor) caps winners while full stop-outs hit `-0.55%` plus adverse slippage and taxes. A drop in win rate below `63%` out-of-sample will turn Track B net-negative.
3. **Brokerage Sensitivity (`Kotak Neo ₹0 API` vs. `0.03%` Discount Broker)**:
   - The C++ `PaperBroker` ([`paper_broker.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/execution/paper_broker.hpp#L224-L230)) models **statutory exchange/government taxes only** (`STT 0.025%` sell + `NSE 0.00297%` + `SEBI 0.0001%` + `18% GST` + `Stamp 0.003%` buy = `~₹3.52` per `₹10k` round-trip).
   - On standard discount brokers charging `0.03%` per leg (`+₹6.19` to `+₹8.78` brokerage + GST per round-trip), Track B loses **`67%` of its net profit** (`+₹138.12` → `+₹45.27`).
   - Routing via **Kotak Neo Trade API v2** (`neo-fin-key: neotradeapi`) charges **`₹0` brokerage on API orders**, matching the statutory-only model — provided **both entry and exit (`15:14 IST`) are executed via the API** (manual exits clicked in the mobile app after 30 days incur `₹10`/order, and broker RMS square-offs after `15:15 IST` incur `₹50 + GST`).
4. **Engineering Bottleneck vs. C++ Hot Path**:
   - The C++20 engine processes ticks in `< 2 µs` via lock-free SPSC ring buffers, and the `184` CTest assertions verify **software correctness** (accounting identities, `Tick[t+1]` matching, state recovery).
   - However, in live cloud paper trading, quotes arrive via a **`1.0s` Python REST poll (`~190–210 ms` HTTP latency from GitHub Actions to Indian broker endpoints)**. The C++ nanosecond hot path is not the bottleneck; network geography and `1s` REST sampling are. Moving to a Mumbai VPS with persistent WebSocket streaming is required before any live micro-capital deployment.
5. **30-Day Synthetic Dataset (`data/historical_30days_nse.csv`)**:
   - The 30-day synthetic benchmark (`568,819` ticks; Track A `+22.40%`, PF `16.32`; Track B `+6.08%`, PF `2.72`) is a **software stress test** generated by `scripts/generate_30day_data.py`. It tests multi-day state rollover and kill-switch logic, **not** real market edge.

---

## 🧪 Out-of-Sample Validation Protocol & Pass/Fail Criteria

To evaluate whether DeltaUno possesses genuine statistical edge net of costs:
1. **Strict Parameter Freeze**: No modifications to `include/deltauno/strategy/momentum_breakout.hpp`, `include/deltauno/risk/risk_manager.hpp`, or `config/risk_limits.json` during the OOS collection window starting **`2026-11-02`**.
2. **Sample Size Requirement**: Collect **`>= 60` consecutive live NSE sessions** or **`>= 100` out-of-sample trades per track** (`LIVE_OOS` provenance tag only).
3. **Pre-Committed Pass/Fail Rule (Evaluated at `N = 100` OOS Trades per Track)**:
   - **Pass Criterion 1 (Net Expectancy after All Costs)**: `1.0x` leverage-normalized net expectancy `> +₹10.00 / trade` (`> +0.10% / trade`) after statutory taxes and target broker commission.
   - **Pass Criterion 2 (Baseline Outperformance)**: Outperforms both (a) equal-weighted intraday buy-and-hold (`09:35 IST` buy to `15:14 IST` sell) on the 10-stock universe and (b) time-matched random entries using identical `-0.85% / -0.55%` stops and `15:14 IST` exits.
   - **Pass Criterion 3 (Max Drawdown)**: Maximum cumulative peak-to-trough equity drawdown `<= 6.0%` (`₹600` on `₹10,000` at `1.0x` leverage).

---

## 📚 Academic Literature Context (Inspirations vs. Implementation Differences)

Literature references below motivate the feature forms used in the engine; **citing a paper does not validate the specific thresholds or parameters chosen in DeltaUno**:

| # | Academic Paper | What the Paper Actually Shows | How DeltaUno Adapts It (and Where It Diverges) |
| :---: | :--- | :--- | :--- |
| **1** | **Cont, Kukanov & Stoikov (2014)**, *The Price Impact of Order Book Events*, JFE | Contemporaneous 10-second price changes are linearly related to top-of-book Order Flow Imbalance ($R^2 \approx 65\%$ in-interval, not a 15-minute ahead forecast). | **Heuristic Extension**: Computes 5-level depth-weighted OFI (`{1.0, 0.7, 0.5, 0.35, 0.25}`) as an entry confirmation filter alongside price/VWAP breakouts rather than a standalone multi-minute return predictor. |
| **2** | **Kolm, Turiel & Westray (2023)**, *Deep Order Flow Imbalance*, Math. Finance | Multi-horizon L2 OFI vectors trained with deep neural networks predict short-horizon mid-price returns across sub-second/second horizons. | **Simplified Linear Proxy**: Replaces trained neural networks with a fixed 3-horizon weighted average ($0.5\,\text{OFI}_1 + 0.3\,\text{OFI}_5 + 0.2\,\text{OFI}_{20}$) over 1-second REST snapshots. |
| **3** | **Zhang, Zohren & Roberts (2019)**, *DeepLOB*, IEEE TSP | CNN-LSTM architectures extract spatial-temporal features from raw L2 limit order books. | **Deliberate Non-Adoption**: Avoids deep neural weights on small sample sizes; uses fixed analytical book ratios instead. |
| **4** | **Stoikov (2018)**, *The Micro-Price*, Quantitative Finance | Queue-imbalance-adjusted micro-price is a better short-term fair-value estimator than simple mid-price. | **Direct Adoption**: Requires `weighted_mid_price >= mid_price` for Longs and `<= mid_price` for Shorts. |
| **5** | **Gao, Han, Li & Zhou (2018)**, *Market Intraday Momentum*, JFE | First-30-minute S&P 500 ETF return predicts the **last-30-minute (`15:00–15:30`) market-index return** driven by late institutional rebalancing. | **Operational Adaptation**: Applies the first-30-minute directional filter (`r_{30m}`) to individual high-beta stocks in Track C (`OMNI`), and sets intraday auto-squareoff at **`15:14:00 IST`** primarily to exit 1 minute before Indian broker MIS RMS auto-liquidation (`15:15 IST`, which carries a ₹50+GST penalty). |
| **6** | **Easley, López de Prado & O'Hara (2012)**, *Flow Toxicity and Liquidity*, RFS | Volume-synchronized probability of informed trading (VPIN) measures adverse selection in high-frequency order flow. | **Coarse Operational Proxy**: Does not compute VPIN; instead blocks entries during scheduled macro announcements (`calendar_events.json`) and within `1.5%` of NSE circuit bands. |
| **7** | **Pelster & Hofmann (2018)**, *About the Risks of Social Trading*, JBF | Retail copy-traders suffer from a stronger disposition effect (holding losers, selling winners early). | **Design Motivation Only**: Motivates mechanical stop-loss / break-even ratchets (`+0.90%` Long / `+0.45%` Short trigger) and sanity-checking external webhook alerts against local book imbalance (`signal_ingestor.hpp`). |
| **8** | **Barber, Huang, Odean & Schwarz (2022)**, *Attention-Induced Trading*, JoF | Retail investors herd into attention-grabbing stocks experiencing extreme volume/price spikes, earning negative subsequent returns. | **Heuristic Guard**: Blocks entries when single-tick volume exceeds `4.0x` rolling average or price has already jumped `> 2.0%`, plus a 45-minute per-symbol cooldown. |
| **9** | **Almgren & Chriss (2000)**, *Optimal Execution of Portfolio Transactions*, J. Risk | Models temporary and permanent market impact costs under risk aversion. | **Simplified Slippage & Liquidity Filter**: Models execution cost as opposite-side best bid/ask fill on `Tick[t+1]` plus depth-sweep penalty when `order_qty > top_book_qty`, and requires `L1 Qty >= 50` shares. |
| **10** | **Moskowitz, Ooi & Pedersen (2012)**, *Time Series Momentum*, JFE | Volatility-scaled cross-asset monthly time-series momentum (`1–12` month horizons). | **Intraday Adaptation**: Uses volatility/stop-distance inverse position sizing (`RiskCap / |Entry - SL|`) and independent Long + Short slots in Track C. |

---

## 🛡️ Implemented Engine Controls (Verified by CTest Suite)

The 9 CTest suites (`184` assertions) verify that the C++20 engine deterministically enforces the following rules:
1. **Fixed-Risk Position Sizing**: $\text{Qty} = \min(\lfloor \text{RiskCap} / |\text{Entry} - \text{SL}| \rfloor, \lfloor \text{NotionalCap} / \text{Entry} \rfloor)$.
2. **Top-of-Book L1 Liquidity Filter**: Requires `best_ask_qty() >= 50` for Longs and `best_bid_qty() >= 50` for Shorts.
3. **NIFTY 50 Macro Regime Filter**: Blocks new Long entries when `NIFTY 50 <= -0.35%` on the day.
4. **Daily Drawdown Kill-Switch**: Halts new entries if intraday net or peak-to-trough drawdown reaches **₹500.00 (5%)**.
5. **Break-Even Stop Ratchet**: Moves stop loss to `Entry ± 0.15%` once unrealized gain reaches `+0.90%` (Long) or `+0.45%` (Short).
6. **15:14:00 IST Auto-Squareoff & Pre-`#SESSION_END` Runner Guard**: Flattens all open intraday positions at `15:14:00 IST` (1 minute before `15:15 IST` broker RMS square-off), backed by a Python `finally:` safety net if a cloud runner exits early.
7. **Circuit Band Guard (`1.5%`) & Macro Event Freeze**: Blocks entries near upper/lower circuits or during scheduled RBI/Budget/FOMC windows.
8. **Zero Look-Ahead Bias (`Tick[t+1]` Matching)**: Orders created on `Tick[t]` fill strictly on `Tick[t+1]` (`timestamp_ns > created_time_ns`) at the opposite side of the book (`best_ask` for `BUY`/`COVER`, `best_bid` for `SELL`/`SHORT`).
9. **Atomic Crash Recovery (`deltauno_state.json`)**: Persists open position state via `.tmp` + rename.

---

## 🏷️ Release History & Semantic Versioning Tags

| Git Tag | Commit | Release Summary |
| :--- | :---: | :--- |
| **`v3.3.0`** | `HEAD` | **Parameter Freeze, Provenance Tagging (`REPLAY_V3.2` vs `LIVE_OOS`), Kotak Neo `₹0` API Support & `15:14 IST` Runner Fix**: Adds `1.0x` leverage-normalized returns, net expectancy (`₹/trd`), and `₹0` Kotak Neo vs `0.03%` brokerage tracking to Google Sheets; adds `--provider kotakneo` (`TOTP + MPIN` v2 API); fixes the `350m` (`15:05 IST`) GitHub Actions cutoff to `355m` (`15:15 IST`) with a pre-`#SESSION_END` auto-squareoff safety net; documents in-sample vs out-of-sample split and methodology limitations. |
| **`v3.2.0`** | `8d37551` | **15:14:00 IST Pre-RMS Auto-Squareoff Update**: Moves engine auto-squareoff from `15:05:00` to `15:14:00 IST` across all 3 tracks (`1` minute prior to retail broker `15:15` MIS RMS liquidations). |
| **`v3.1.1`** | `58de41d` | **Cloud-Native Morning Google Apps Script Trigger & Watchdog**: Adds Google Cloud time-driven triggers (`autoMorningGitHubDispatch` + `cloudMorningWatchdog`). |
| **`v3.1.0`** | `10f7fd3` | **In-Sample Parameter Iteration (`2.15x` MIS Omni Sizing, Harvester `-0.55%` SL & 10-Symbol Universe)**: Expands universe to 10 NSE F&O stocks and introduces `2.15x` MIS sizing on Track C. |
| **`v3.0.0`** | `13572e4` | **3-Track Architecture (`Track A Compounder` + `Track B Harvester` + `Track C Bi-Directional Omni`)**: Adds Dual-Slot (`1 Long + 1 Short`) book and `L1 Qty >= 50` liquidity filter. |
| **`v2.5.0`** | `21a4201` | **Intraday Short Selling (`Side::SELL` / `COVER`) & Google Sheets v2.5 3-Track Studio**: Symmetric short-selling broker/risk engine and AngelOne ⇄ Upstox failover. |
| **`v2.0.0`** | `b008bb2` | **Live NSE Cloud Paper Trading (`--mode both` + `--live-tail`)**: GitHub Actions runner, AngelOne/Upstox REST poller, and Google Sheets webhook bridge. |
| **`v1.0.0`** | `36e3aad` | **Core C++20 Engine & 184-Assertion Test Suite**: SPSC/MPMC ring buffers, 5-level L2 order book, risk manager, and paper broker. |

---

## 🚀 Building & Running

### Build & Run CTest Suite (`9/9` Suites, `184` Assertions)
```powershell
cmake -G "MinGW Makefiles" -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

### Run Offline Replay (`--mode both`)
```powershell
.\build\deltauno.exe --mode both --input data/historical_30days_nse.csv --compounder-capital 10000 --harvester-capital 10000 --omni-capital 10000
```

### Run Live NSE Paper Trading Orchestrator
```powershell
# 1. End-to-end live-tail self-test
python scripts/run_live_paper_trading.py --self-test

# 2. Test live broker L2 quote connection (auto-detects Kotak Neo, AngelOne, Upstox, Zerodha, or Yahoo)
python scripts/run_live_paper_trading.py --provider auto --test-connection

# 3. Run full NSE session (09:15 - 15:15 IST, auto-squareoff at 15:14:00 IST)
python scripts/run_live_paper_trading.py --provider auto --mode both
```
