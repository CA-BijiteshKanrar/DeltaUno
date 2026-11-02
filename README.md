# DeltaUno: C++20 3-Track Intraday Paper-Trading & Microstructure Research Laboratory

A deterministic **C++20 intraday trading engine and forward-testing laboratory** built to evaluate order-flow imbalance (OFI) and momentum hypotheses on Indian Equities (**NSE/BSE**, ₹0.05 tick precision) using a ring-fenced **₹10,000 (~$115) per-track research capital baseline**.

DeltaUno runs **three parallel strategy tracks (`--mode both`: Track A Compounder, Track B Harvester, Track C Bi-Directional Omni)** on a shared 5-level Level-2 quote stream via **persistent Binary L2 WebSocket streaming (`wss://smartapisocket.angelone.in/smart-stream` Mode 3 Snap Quote) piped directly into C++ memory (`--stdin-stream`)**, with automatic multi-broker REST failover (**Kotak Neo Trade API v2**, **AngelOne SmartAPI**, **Upstox API v2**), a **Taken + Untaken Signal & Forward-Return Logger** (`data/signal_forward_returns_YYYYMMDD.csv`), and automated telemetry to Google Sheets.

> [!IMPORTANT]
> **Research Status, CI-Enforced Parameter Freeze & Provenance Split (`v3.3.0`)**:
> - **In-Sample Fitting Window (`2026-10-26` to `2026-10-30`, `REPLAY_V3.2`)**: Those `5` sessions (`534,900` recorded ticks) were used to choose strategy rules, universe selection, and the `15:14:00 IST` exit timing, and are strictly **in-sample fitting data**.
> - **Shakedown Session (`2026-11-02`, `SHAKEDOWN_LIVE` + `REPLAY_EOD_FIX`)**: On `2026-11-02`, `4` trades (`+₹105.70` net) closed genuinely live before `15:05 IST`, while `+₹234.37` of the session's `+₹340.07` (`68.9%`) came from `2` `HINDALCO` exits (`Track A +₹102.86`, `Track C +₹131.51`) that were replayed after fixing the `350`-minute runner timer bug (`REPLAY_EOD_FIX`). Accordingly, `2026-11-02` is classified as an **infrastructure shakedown session**, not clean out-of-sample evidence.
> - **Clean Out-of-Sample (`LIVE_OOS`) Window (`2026-11-03` Onward)**: Starting **`2026-11-03`**, all strategy and risk files are frozen and verified on every CI run via `git diff --exit-code v3.2.0 -- include/deltauno/strategy/ include/deltauno/risk/ config/risk_limits.json config/instruments.json config/calendar_events.json`. No strategy edge is claimed until **60–100+ out-of-sample sessions (`>= 100` trades and `>= 300` logged signals per track)** are evaluated under the pre-committed pass/fail criteria in [Section 2](#-out-of-sample-validation-protocol--passfail-criteria).

---

## 🏛️ System Architecture (`v3.3.0` Event-Driven WebSocket + `--stdin-stream`)

```text
[ AngelOne SmartStream v2 Binary L2 WebSocket (Mode 3, 379B Little-Endian) ]
  └── Automatic Failover: Kotak Neo v2 / AngelOne / Upstox L2 REST Poll
                          │
                          ├──► Async CSV Journal (data/live_ticks_YYYYMMDD.csv)
                          │
                          ▼ (Zero-Disk OS Pipe: Python stdout ──► C++ std::cin --stdin-stream)
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
                          ├───────────────────────────────────────────────────┐
                          ▼                                                   ▼
          [ Pre-Trade Risk Manager (14 Deterministic Controls) ]   [ Signal & Forward-Return Logger ]
             ├── Fixed-Risk Sizing (1.2%–1.8% risk, 1.0x–2.15x)       ├── Logs EVERY signal (TAKEN,
             ├── NIFTY 50 Regime Filter (Blocks Longs <= -0.35%)      │   SLOT_BUSY, or Risk-Rejected)
             ├── Intraday Drawdown Kill-Switch (₹500 / 5% limit)      └── Computes 1m, 5m, 15m & EOD
             ├── Break-Even Stop Ratchet (+0.90% L / +0.45% S)            forward returns (bps) to
             ├── 15:14:00 IST Auto-Squareoff (Pre-15:15 RMS)              signal_forward_returns.csv
             └── Statutory Friction Filter (Gain >= 2.5x Taxes)
                          │
                          ▼
          [ Paper Broker & Provenance-Tagged Telemetry ]
             ├── Zero Look-Ahead Bias (Tick[t] orders match strictly on Tick[t+1])
             ├── Marketable-Limit Order Price Cap & Timeout Enforcement (SEBI Retail Algo Rule)
             ├── Adverse Bid/Ask Sweep Slippage + Missed-Limit-Fill Counter
             ├── NSE MIS Statutory Taxes + Explicit 0.03% Discount Broker vs. ₹0 Kotak Neo API Audit
             └── Provenance-Tagged Google Sheets Ledger (REPLAY_V3.2 | SHAKEDOWN_LIVE | REPLAY_EOD_FIX | LIVE_OOS)
```

---

## 📊 Empirical Results, Provenance Split & Known Limitations

### 1. 6-Session NSE Recorded Tick Ledger (`2026-10-26` to `2026-11-02` — `714,972` Ticks)

Every trade and session in the ledger is separated into three explicit provenance categories so in-sample backfills and shakedown replays are never mixed with clean out-of-sample results:
- **`REPLAY_V3.2 (IN-SAMPLE)` (`2026-10-26` to `2026-10-30`, 5 sessions, `34` trades)**: Recorded live from AngelOne/Upstox, but replayed after `v3.1`/`v3.2` parameter adjustments (`10`-stock universe expansion, `2.15x` MIS sizing on Track C, `-0.55%` SL on Track B, `15:14 IST` square-off). Treated strictly as **in-sample fitting data**.
- **`SHAKEDOWN_LIVE` + `REPLAY_EOD_FIX` (`2026-11-02`, 1 shakedown session, `6` trades)**: Executed under frozen `v3.2` parameters, where **`4` trades (`+₹105.70` net across all 3 tracks)** closed genuinely live before `15:05 IST` (`SHAKEDOWN_LIVE`), and **`2` `HINDALCO` trades (`+₹234.37` net — `68.9%` of the day's `+₹340.07` PnL)** were squared off via `REPLAY_EOD_FIX` at `15:14:01 IST` after fixing the `350`-minute GitHub Actions workflow timer.
- **`LIVE_OOS` (`2026-11-03` onward)**: Clean out-of-sample forward validation window with CI-enforced parameter freeze (`git diff --exit-code v3.2.0`).

| Strategy Track | Leverage & Sizing | In-Sample Replay (`Oct 26–30`, 5d) | Shakedown (`Nov 02`: Live / Replay) | 6-Day Ending Equity | Net PnL (**Kotak Neo `₹0` Brk**) | Net PnL (`0.03%` Brk) | `1.0x` Norm Return | Avg Win / Avg Loss | Net Expectancy (`₹/trd`: `₹0` / `0.03%`) | Win Rate (`40` Trades) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Track C: Bi-Directional Omni (`OMNI`)** | `2.15x` MIS (`1.80%` Risk) | `+₹739.09` (`11T`) | `+₹25.06` (`1T` Live) / `+₹131.51` (`1T` Replay) | `₹10,895.66` | **`+₹895.66 (+8.96%)`** | `+₹781.52 (+7.82%)` | **`+4.17%`** | `+₹105.51` / `-₹132.46` | **`+₹68.90`** / `+₹60.12` | `84.6%` (`11W/2L`) |
| **Track A: Compounder** | `1.0x` Cash (`1.20%` Risk) | `+₹88.40` (`10T`) | `+₹11.12` (`1T` Live) / `+₹102.86` (`1T` Replay) | `₹10,202.38` | **`+₹202.38 (+2.02%)`** | `+₹128.10 (+1.28%)` | **`+2.02%`** | `+₹37.61` / `-₹45.38` | **`+₹16.86`** / `+₹10.68` | `75.0%` (`9W/3L`) |
| **Track B: Harvester** | `1.0x` Cash (`1.20%` Risk) | `+₹68.60` (`13T`) | `+₹69.52` (`2T` Live) / `₹0.00` (`0T` Replay) | `₹10,138.12` | **`+₹138.12 (+1.38%)`** | `+₹45.27 (+0.45%)` | **`+1.38%`** | `+₹32.50` / `-₹54.85` | **`+₹9.21`** / `+₹3.02` | `73.3%` (`11W/4L`) |
| **Combined (`₹30,000` Start)** | All 3 Tracks | `+₹896.09` (`34T`) | `+₹105.70` (`4T` Live) / `+₹234.37` (`2T` Replay) | `₹31,236.16` | **`+₹1,236.16 (+4.12%)`** | `+₹954.89 (+3.18%)` | **`+2.52%`** | — | **`+₹30.90`** / `+₹23.87` | `77.5%` (`31W/9L`) |

---

### 2. All-10-Symbol Intraday (`09:35 IST → 15:14 IST`) Market Baseline (`Oct 26` – `Nov 02`)

To avoid selection bias from excluding untraded stocks or comparing intraday strategies against multi-day overnight holds, the benchmark below measures an **equal-weighted intraday buy-and-hold (`09:35 IST` entry to `15:14 IST` exit, net of `0.0352%` statutory MIS taxes per day) across all 10 whitelisted symbols** (`config/instruments.json`):

| Date | `TATASTEEL` | `SBIN` | `ITC` | `COALINDIA` | `ZOMATO` | `TATAMOTORS` | `BEL` | `TRENT` | `DLF` | `HINDALCO` | Equal-Wt 10-Symbol Intraday Net (`1.0x`) |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`2026-10-26`** | `+1.84%` | `+1.29%` | `+0.92%` | `+0.88%` | `+3.42%` | `+1.15%` | `+1.04%` | `+2.65%` | `+1.91%` | `+1.70%` | **`+1.68%` (`+₹168.00`)** |
| **`2026-10-27`** | `-0.42%` | `-0.31%` | `+0.18%` | `-0.54%` | `+0.41%` | `-0.28%` | `-0.49%` | `+0.12%` | `-0.35%` | `-0.12%` | **`-0.18%` (`-₹18.00`)** |
| **`2026-10-28`** | `+0.51%` | `+0.22%` | `+0.95%` | `+0.19%` | `+0.64%` | `+0.31%` | `+0.15%` | `+0.48%` | `+0.08%` | `+0.17%` | **`+0.37%` (`+₹37.00`)** |
| **`2026-10-29`** | `+0.92%` | `+0.64%` | `+1.88%` | `+0.76%` | `+1.65%` | `+0.98%` | `+0.58%` | `+2.84%` | `+1.72%` | `+1.13%` | **`+1.31%` (`+₹131.00`)** |
| **`2026-10-30`** | `-0.19%` | `-0.11%` | `+0.34%` | `+0.12%` | `+0.25%` | `-0.08%` | `-0.22%` | `+0.36%` | `-0.14%` | `-0.13%` | **`+0.02%` (`+₹2.00`)** |
| **`2026-11-02`** | `+0.77%` | `+0.27%` | `+0.32%` | `+0.44%` | `+0.62%` | `+0.52%` | `+0.18%` | `+0.84%` | `+1.12%` | `+1.75%` | **`+0.68%` (`+₹68.28`)** |
| **6-Day Sum** | **`+3.43%`** | **`+2.00%`** | **`+4.59%`** | **`+1.85%`** | **`+6.99%`** | **`+2.60%`** | **`+1.24%`** | **`+7.29%`** | **`+4.34%`** | **`+4.50%`** | **`+3.88%` (`+₹388.28` on `₹10k`)** |

- **Key Takeaway against the 10-Symbol Intraday Baseline (`+3.88%`)**:
  - **Track A (`+2.02%`) and Track B (`+1.38%`) both underperformed passive equal-weighted intraday buy-and-hold (`+3.88%`)** over the 6-session window, largely because they stayed in cash ~55–65% of the session during a broad bullish run and capped gains early in Track B.
  - **Track C (`+4.17%` normalized to `1.0x` leverage)** beat the 10-symbol intraday baseline by only **`+0.29%` (`+29 bps`)**, which is well within statistical noise over 13 trades.

---

### 3. Honest Assessment of Strategy Shape, Execution Realism & Costs

1. **Extreme Profit Concentration in `Track C` (`100.7%` in 4 Trades)**:
   - Track C's headline `+8.96%` (`+₹895.66`) return is magnified `2.15x` by MIS intraday leverage (`1.80%` risk per slot, `+4.17%` at `1.0x`).
   - More importantly, its four largest trades (`ITC` Short `+₹221.30`, `ZOMATO` Long `+₹227.44`, `ITC` Long `+₹321.58`, and the replayed `HINDALCO` Long `+₹131.51`) add up to **`+₹901.83` — which is `100.7%` of Track C's total net profit of `+₹895.66`**. The remaining **`9` trades in Track C netted `-₹6.17` combined**.
2. **Track B Already Misses the `₹10/trade` Expectancy Bar & Has Negative Win/Loss R:R**:
   - Track B (`HARVESTER`) has an average win (`+₹32.50`) that is smaller than its average loss (`-₹54.85`, R:R `0.59`).
   - Even at `₹0` brokerage, Track B's net expectancy is **`+₹9.21/trade`** — **already below the `+₹10.00/trade` threshold** — and drops to **`+₹3.02/trade`** (`-67%`) under `0.03%` discount-brokerage.
3. **Kotak Neo `₹0` API Brokerage Verification & Operational Caveats**:
   - Under Kotak Neo's Trade Free plans (effective November 1, 2025), orders placed via the **Kotak Neo Trade API v2 (`neo-fin-key: neotradeapi`)** carry **`₹0` brokerage and `₹0` API platform fees** (whereas app/web intraday orders cost `₹10/order` after the first 30 days).
   - **Four mandatory caveats apply**:
     1. *"At Present" Pricing*: Broker tariff sheets state `₹0` API brokerage applies *"at present"* and can be revised by Kotak at any time.
     2. *Static IP Requirement*: SEBI/NSE retail algo rules require registering a **whitelisted static IP** (e.g., a Mumbai VPS Elastic IP) for automated API order placement.
     3. *No Bracket Orders (BO)*: Kotak charges standard brokerage on the square-off leg of Bracket Orders; both entry and exit (`15:14:00 IST`) must be submitted as **plain API orders**.
     4. *No Manual App Overrides*: Closing an API-opened trade manually in the Kotak mobile app/web terminal after Day 30 triggers `₹10/order` brokerage, and missing the `15:14 IST` exit triggers `₹50 + 18% GST` RMS auto-squareoff charges at `15:15 IST`.
4. **SEBI Retail Algo Limit-Order Rule & Marketable-Limit Modeling (`paper_broker.hpp`)**:
   - SEBI retail algo regulations prohibit unpriced market orders (`OrderType::MARKET`) over broker APIs; live execution must use **marketable limit orders** (`limit_price = best_ask + cap` for `BUY`, `best_bid - cap` for `SELL`).
   - Unlike idealized market orders, limit orders in fast breakouts face **adverse selection** (missing fills when price runs away cleanly, while filling 100% of the time when the breakout immediately reverses).
   - `PaperBroker` ([`paper_broker.hpp`](file:///d:/personal-proj/DeltaUno/include/deltauno/execution/paper_broker.hpp#L118-L162)) models marketable-limit execution via `set_marketable_limit_bounds(max_slippage_bps, timeout_ns)` and tracks `missed_limit_fills_count()` whenever `Tick[t+1]` gaps past the limit price cap or expires unfilled.
5. **Eliminating the `1s` REST Poll + Disk-Tail Bottleneck (`Option A` Implemented)**:
   - Previously, Python polled REST endpoints every `1.0s` (`~200 ms` HTTP latency) and wrote rows to a CSV file tailed by C++.
   - In `v3.3.0`, `LiveQuoteProvider.stream_angelone_websocket()` connects to **AngelOne SmartStream v2 (`wss://smartapisocket.angelone.in/smart-stream`, Mode 3 Binary 5-Level Snap Quote)** and writes ticks directly into **`deltauno.exe --stdin-stream`** over an in-memory OS pipe (`stdin` → `SPSCRingBuffer`), while asynchronously journaling to `data/live_ticks_YYYYMMDD.csv`.
6. **Taken + Untaken Signal & Forward-Return Logger (`data/signal_forward_returns_YYYYMMDD.csv`)**:
   - Because each track executes only ~2 trades/day (`~50` sessions for `100` trades), `src/main.cpp` emits `[LIVE_EVENT] {"type":"SIGNAL",...}` for **every strategy signal generated across all 3 tracks** — whether `TAKEN`, skipped because a slot was already occupied (`SLOT_BUSY`), or blocked by a risk gate (`NIFTY_BEAR_REGIME`, `L1_DEPTH_INSUFFICIENT`, `MAX_NOTIONAL_EXCEEDED`, etc.).
   - `scripts/run_live_paper_trading.py` tracks the subsequent `1m`, `5m`, `15m`, and `15:14 IST (EOD)` forward returns (in basis points) for every logged signal and writes `data/signal_forward_returns_YYYYMMDD.csv`, multiplying the statistical sample size without touching frozen strategy parameters.

---

## 🧪 Out-of-Sample Validation Protocol & Pass/Fail Criteria

To evaluate whether DeltaUno possesses genuine statistical edge net of costs:
1. **CI-Enforced Parameter Freeze (`2026-11-03` Onward)**: Every workflow run executes `git diff --exit-code v3.2.0 -- include/deltauno/strategy/ include/deltauno/risk/ config/risk_limits.json config/instruments.json config/calendar_events.json` and fails the build if any strategy or risk parameter is modified during the OOS collection window.
2. **Sample Size Requirement**: Collect **`>= 60` consecutive live NSE sessions**, **`>= 100` out-of-sample executed trades per track** (`LIVE_OOS` provenance tag only, excluding `2026-11-02` shakedown and all replays), and **`>= 300` logged signals** in `signal_forward_returns_YYYYMMDD.csv`.
3. **Pre-Committed Pass/Fail Rule (Evaluated at `N >= 100` `LIVE_OOS` Trades per Track)**:
   - **Pass Criterion 1 (Point Expectancy after All Costs)**: `1.0x` leverage-normalized net expectancy `> +₹10.00 / trade` (`> +0.10% / trade`) after statutory taxes and target broker commission (*Note: Track B at `+₹9.21/trd` currently fails this bar*).
   - **Pass Criterion 2 (Statistical Significance / 95% Confidence Bound)**: Because Track C's per-trade standard deviation is ~3x larger than Track A/B's (`₹10/trd` over `100` trades is only `~1.5` standard errors for Track C), a track must achieve a **one-sided 95% confidence interval lower bound above zero ($t\text{-stat} = \frac{\bar{x}}{s / \sqrt{N}} \ge 1.96$) net of all costs**, both on executed trades and on `15m`/`EOD` forward returns in `signal_forward_returns_YYYYMMDD.csv`.
   - **Pass Criterion 3 (All-10-Symbol Intraday Baseline Outperformance)**: Outperforms both (a) the **all-10-symbol equal-weighted intraday buy-and-hold baseline (`09:35 IST → 15:14 IST`, net of statutory MIS taxes)** and (b) time-matched random entries using identical stops and `15:14 IST` exits.
   - **Pass Criterion 4 (Max Drawdown)**: Maximum cumulative peak-to-trough equity drawdown `<= 6.0%` (`₹600` on `₹10,000` at `1.0x` leverage).

---

## 📚 Academic Literature Context (Inspirations vs. Implementation Differences)

Literature references below motivate the feature forms used in the engine; **citing a paper does not validate the specific thresholds or parameters chosen in DeltaUno**:

| # | Academic Paper | What the Paper Actually Shows | How DeltaUno Adapts It (and Where It Diverges) |
| :---: | :--- | :--- | :--- |
| **1** | **Cont, Kukanov & Stoikov (2014)**, *The Price Impact of Order Book Events*, JFE | Contemporaneous 10-second price changes are linearly related to top-of-book Order Flow Imbalance ($R^2 \approx 65\%$ in-interval, not a 15-minute ahead forecast). | **Heuristic Extension**: Computes 5-level depth-weighted OFI (`{1.0, 0.7, 0.5, 0.35, 0.25}`) as an entry confirmation filter alongside price/VWAP breakouts rather than a standalone multi-minute return predictor. |
| **2** | **Kolm, Turiel & Westray (2023)**, *Deep Order Flow Imbalance*, Math. Finance | Multi-horizon L2 OFI vectors trained with deep neural networks predict short-horizon mid-price returns across sub-second/second horizons. | **Simplified Linear Proxy**: Replaces trained neural networks with a fixed 3-horizon weighted average ($0.5\,\text{OFI}_1 + 0.3\,\text{OFI}_5 + 0.2\,\text{OFI}_{20}$). |
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
8. **Zero Look-Ahead Bias (`Tick[t+1]` Matching) & Marketable-Limit Bounds**: Orders created on `Tick[t]` fill strictly on `Tick[t+1]` (`timestamp_ns > created_time_ns`) at the opposite side of the book (`best_ask` for `BUY`/`COVER`, `best_bid` for `SELL`/`SHORT`), enforcing optional marketable-limit price caps (`max_marketable_limit_slippage_bps_`) and timeouts (`order_timeout_ns_`) while counting missed limit fills (`missed_limit_fills_count()`).
9. **Atomic Crash Recovery (`deltauno_state.json`)**: Persists open position state via `.tmp` + rename.

---

## 🏷️ Release History & Semantic Versioning Tags

| Git Tag | Commit | Release Summary |
| :--- | :---: | :--- |
| **`v3.3.0`** | `HEAD` | **Binary L2 WebSocket + `--stdin-stream` Pipe, Signal Forward-Return Logger, CI Parameter-Freeze Gate & Provenance Audit**: Streams AngelOne SmartStream v2 Binary L2 ticks directly into `deltauno.exe --stdin-stream`; logs `1m`/`5m`/`15m`/`EOD` forward returns for all taken and untaken signals (`data/signal_forward_returns_YYYYMMDD.csv`); adds marketable-limit order cap/timeout enforcement in `paper_broker.hpp`; reclassifies `2026-11-02` as a Shakedown session (`68.9%` `REPLAY_EOD_FIX`) with clean `LIVE_OOS` starting `2026-11-03` enforced by CI `git diff --exit-code v3.2.0`; benchmarks against the all-10-symbol intraday (`09:35 -> 15:14 IST`, `+3.88%`) baseline. |
| **`v3.2.0`** | `8d37551` | **15:14:00 IST Pre-RMS Auto-Squareoff Freeze Tag**: Moves engine auto-squareoff from `15:05:00` to `15:14:00 IST` across all 3 tracks (`1` minute prior to retail broker `15:15` MIS RMS liquidations). Serves as the frozen baseline reference tag for CI `git diff --exit-code v3.2.0`. |
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

### Run Live NSE Paper Trading Orchestrator (Binary L2 WebSocket + `--stdin-stream`)
```powershell
# 1. End-to-end live-tail self-test (verifies C++ engine + signal logger + Google Sheets sync)
python scripts/run_live_paper_trading.py --self-test

# 2. Test live broker L2 quote connection (auto-detects Kotak Neo, AngelOne, Upstox, Zerodha, or Yahoo)
python scripts/run_live_paper_trading.py --provider auto --test-connection

# 3. Run full NSE session (09:15 - 15:15 IST, WebSocket -> --stdin-stream, auto-squareoff at 15:14:00 IST)
python scripts/run_live_paper_trading.py --provider auto --mode both
```
