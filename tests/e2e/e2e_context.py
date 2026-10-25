#!/usr/bin/env python3
"""
DeltaUno E2E Testing Context & Opaque-Box Test Harness
Provides engine execution wrappers, table output parsers, log analyzers,
and the independent Indian Statutory Regulatory Oracle.
"""

import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone, timedelta
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any, Union

# Root repository directory
REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DATA_30DAY = REPO_ROOT / "data" / "historical_30days_nse.csv"
DATA_SAMPLE = REPO_ROOT / "data" / "sample_nse_ticks.csv"
CONFIG_DIR = REPO_ROOT / "config"

IST_OFFSET = timedelta(hours=5, minutes=30)


def safe_unlink(path: Path, max_retries: int = 5, delay: float = 0.03):
    """Safely unlink a file on Windows with retry backoff."""
    if not path.exists():
        return
    for attempt in range(max_retries):
        try:
            path.unlink()
            return
        except OSError:
            time.sleep(delay * (1.5 ** attempt))


def safe_cleanup_tempdir(td: tempfile.TemporaryDirectory, retries: int = 5, base_delay: float = 0.05):
    """Safely cleanup a TemporaryDirectory on Windows with backoff and chmod fallback."""
    for attempt in range(retries):
        try:
            td.cleanup()
            return
        except OSError:
            time.sleep(base_delay * (1.5 ** attempt))
    # Fallback: force writeable permissions on all child items
    try:
        p = Path(td.name)
        if p.exists():
            for root, dirs, files in os.walk(p):
                for f in files:
                    try:
                        fp = Path(root) / f
                        fp.chmod(0o777)
                        fp.unlink(missing_ok=True)
                    except OSError:
                        pass
            shutil.rmtree(p, ignore_errors=True)
    except Exception:
        pass


def ist_to_utc_ns(ymd: str, hms: str) -> int:
    """Convert IST date and time string to UTC epoch nanoseconds."""
    dt_str = f"{ymd} {hms}"
    dt = datetime.strptime(dt_str, "%Y-%m-%d %H:%M:%S")
    # IST is UTC + 5:30
    tz_ist = timezone(IST_OFFSET)
    dt_ist = dt.replace(tzinfo=tz_ist)
    dt_utc = dt_ist.astimezone(timezone.utc)
    return int(dt_utc.timestamp() * 1_000_000_000)


def utc_ns_to_ist_str(ts_ns: int) -> str:
    """Convert epoch nanoseconds to IST formatted string."""
    sec = ts_ns / 1_000_000_000.0
    dt_utc = datetime.fromtimestamp(sec, tz=timezone.utc)
    dt_ist = dt_utc + IST_OFFSET
    return dt_ist.strftime("%Y-%m-%d %H:%M:%S")


def find_engine_executable() -> Path:
    """Locate the built deltauno executable across standard build directory layouts."""
    candidates = [
        REPO_ROOT / "build" / "deltauno.exe",
        REPO_ROOT / "build" / "deltauno",
        REPO_ROOT / "build" / "Release" / "deltauno.exe",
        REPO_ROOT / "build" / "Debug" / "deltauno.exe",
        REPO_ROOT / "deltauno.exe",
        REPO_ROOT / "deltauno",
    ]
    for c in candidates:
        if c.is_file():
            return c
    raise FileNotFoundError(
        f"DeltaUno engine executable not found. Checked: {[str(c) for c in candidates]}"
    )


# ==============================================================================
# INDEPENDENT STATUTORY REGULATORY ORACLE
# ==============================================================================

class IndianStatutoryOracle:
    """
    Independent regulatory clearing oracle implementing Indian statutory charges
    for NSE cash equity intraday trading (zero-brokerage MIS regime).
    """
    RATE_STT = 0.00025            # 0.025% on intraday sell leg turnover
    RATE_NSE_TURNOVER = 0.0000297  # 0.00297% on round-trip turnover
    RATE_SEBI_TURNOVER = 0.000001  # INR 10 per crore (0.0001% of total turnover)
    RATE_GST = 0.18                # 18% on (Exchange + SEBI charges)
    RATE_STAMP_DUTY = 0.00003      # 0.003% on buy leg turnover

    @staticmethod
    def to_paise(val_inr: float) -> int:
        return int(val_inr * 100.0 + 0.5) if val_inr >= 0 else int(val_inr * 100.0 - 0.5)

    @staticmethod
    def to_inr(val_paise: int) -> float:
        return float(val_paise) / 100.0

    @classmethod
    def compute_trade_taxes(cls, entry_price_inr: float, exit_price_inr: float, qty: int) -> Dict[str, float]:
        """Compute exact statutory fees and PnL for a completed round-trip trade."""
        entry_paise = cls.to_paise(entry_price_inr)
        exit_paise = cls.to_paise(exit_price_inr)

        buy_val = cls.to_inr(entry_paise * qty)
        sell_val = cls.to_inr(exit_paise * qty)
        turnover = buy_val + sell_val
        gross_pnl = sell_val - buy_val

        stt = sell_val * cls.RATE_STT
        exch = turnover * cls.RATE_NSE_TURNOVER
        sebi = turnover * cls.RATE_SEBI_TURNOVER
        gst = (exch + sebi) * cls.RATE_GST
        stamp = buy_val * cls.RATE_STAMP_DUTY
        total_friction = stt + exch + sebi + gst + stamp
        net_pnl = gross_pnl - total_friction

        return {
            "buy_val": buy_val,
            "sell_val": sell_val,
            "turnover": turnover,
            "gross_pnl": gross_pnl,
            "stt": stt,
            "exch": exch,
            "sebi": sebi,
            "gst": gst,
            "stamp": stamp,
            "total_friction": total_friction,
            "net_pnl": net_pnl,
        }

    @classmethod
    def compute_pretrade_estimate(cls, entry_price_inr: float, qty: int) -> Dict[str, float]:
        """Compute estimated statutory friction for pre-trade risk evaluation."""
        entry_paise = cls.to_paise(entry_price_inr)
        trade_val = cls.to_inr(entry_paise * qty)
        turnover = trade_val * 2.0

        stt = trade_val * cls.RATE_STT
        exch = turnover * cls.RATE_NSE_TURNOVER
        sebi = turnover * cls.RATE_SEBI_TURNOVER
        gst = (exch + sebi) * cls.RATE_GST
        stamp = trade_val * cls.RATE_STAMP_DUTY
        total_fees = stt + exch + sebi + gst + stamp

        return {
            "trade_val": trade_val,
            "turnover": turnover,
            "stt": stt,
            "exch": exch,
            "sebi": sebi,
            "gst": gst,
            "stamp": stamp,
            "total_fees": total_fees,
        }


# ==============================================================================
# STRUCTURED DATA TYPES & PARSERS
# ==============================================================================

@dataclass
class SessionAuditRow:
    day: int
    date_ist: str
    trades: int
    wins: int
    losses: int
    scratches: int
    win_rate_pct: float
    gross_pnl: float
    friction: float
    net_pnl: float
    intra_dd: float
    ending_equity: float
    kill_switch: str


@dataclass
class KillSwitchRow:
    day: int
    date_ist: str
    trades: int
    starting_equity: float
    peak_equity: float
    trough_equity: float
    max_intra_dd: float
    max_intra_dd_pct: float
    status: str


@dataclass
class EquityCurveRow:
    day: int
    date_ist: str
    trades: int
    closing_equity: float
    daily_return_pct: float
    cum_return_pct: float
    hwm: float
    drawdown_from_hwm: float
    drawdown_pct: float


@dataclass
class TradeEvent:
    side: str
    symbol: str
    quantity: int
    price: float
    sl: float = 0.0
    tp: float = 0.0
    gross_pnl: float = 0.0
    taxes: float = 0.0
    net_pnl: float = 0.0
    balance: float = 0.0
    mae: float = 0.0
    mfe: float = 0.0


@dataclass
class EngineRunResult:
    returncode: int
    stdout: str
    stderr: str
    log_lines: List[str] = field(default_factory=list)
    session_rows: List[SessionAuditRow] = field(default_factory=list)
    session_total: Dict[str, Any] = field(default_factory=dict)
    statutory_charges: Dict[str, Any] = field(default_factory=dict)
    kill_switch_rows: List[KillSwitchRow] = field(default_factory=list)
    equity_curve_rows: List[EquityCurveRow] = field(default_factory=list)
    statistical_metrics: Dict[str, Any] = field(default_factory=dict)
    trades: List[TradeEvent] = field(default_factory=list)
    rollover_events: List[Tuple[int, float]] = field(default_factory=list)
    rejections: List[str] = field(default_factory=list)
    vetoes: List[str] = field(default_factory=list)
    state_journal: Optional[Dict[str, Any]] = None
    state_journal_raw: Optional[str] = None


def parse_engine_output(stdout_str: str, log_lines: List[str]) -> EngineRunResult:
    """Parse DeltaUno CLI stdout and deltauno.log into structured records."""
    res = EngineRunResult(returncode=0, stdout=stdout_str, stderr="", log_lines=log_lines)

    # 1. Parse Session-by-Session Performance Breakdown Table
    session_re = re.compile(
        r"^\s*(\d+)\s+(\d{4}-\d{2}-\d{2})\s+(\d+)\s+(\d+)/(\d+)/(\d+)\s+([\d\.]+)%\s+([+-]?[\d\.]+)\s+INR\s+([+-]?[\d\.]+)\s+INR\s+([+-]?[\d\.]+)\s+INR\s+([\d\.]+)\s+INR\s+([\d\.]+)\s+INR\s+([A-Z_]+)\s*$"
    )
    ttl_re = re.compile(
        r"^\s*TTL\s+(\d+)\s+(\d+)/(\d+)/(\d+)\s+([\d\.]+)%\s+([+-]?[\d\.]+)\s+INR\s+([+-]?[\d\.]+)\s+INR\s+([+-]?[\d\.]+)\s+INR\s+([\d\.]+)\s+Max\s+([\d\.]+)\s+INR\s+(\d+)\s+TRIG"
    )

    # 2. Parse Kill-Switch Audit Table
    ks_re = re.compile(
        r"^\s*(\d+)\s+(\d{4}-\d{2}-\d{2})\s+(\d+)\s+([\d\.]+)\s+([\d\.]+)\s+([\d\.]+)\s+([\d\.]+)\s+\(([\d\.]+)%\)\s+([A-Z_]+)\s*$"
    )

    # 3. Parse Equity Curve Table
    eq_re = re.compile(
        r"^\s*(\d+)\s+(\d{4}-\d{2}-\d{2})\s+(\d+)\s+([\d\.]+)\s+([+-]?[\d\.]+)%\s+([+-]?[\d\.]+)%\s+([\d\.]+)\s+([\d\.]+)\s+\(([\d\.]+)%\)\s*$"
    )

    # 4. Parse Statutory Charges
    stt_re = re.compile(r"Securities Transaction Tax \(STT\).*?-INR\s+([\d\.]+)\s+([\d\.]+)%")
    nse_re = re.compile(r"NSE Transaction Turnover Charges.*?-INR\s+([\d\.]+)\s+([\d\.]+)%")
    gst_re = re.compile(r"Goods & Services Tax \(GST\).*?-INR\s+([\d\.]+)\s+([\d\.]+)%")
    stamp_re = re.compile(r"State Stamp Duty.*?-INR\s+([\d\.]+)\s+([\d\.]+)%")
    sebi_re = re.compile(r"SEBI Regulatory Turnover Fee.*?-INR\s+([\d\.]+)\s+([\d\.]+)%")
    tot_fric_re = re.compile(r"TOTAL STATUTORY FRICTION DRAG.*?-INR\s+([\d\.]+)")
    turnover_re = re.compile(r"TOTAL TRADED TURNOVER \(NOTIONAL\).*?INR\s+([\d\.]+)")
    fric_ratio_re = re.compile(r"FRICTION DRAG RATIO ON PROFIT\s+([\d\.]+)%")

    # 5. Parse Statistical Metrics
    tot_trades_re = re.compile(r"Total Completed Trades\s+:\s+(\d+)")
    win_trades_re = re.compile(r"Winning Trades\s+:\s+(\d+)")
    loss_trades_re = re.compile(r"Losing Trades\s+:\s+(\d+)")
    scratch_trades_re = re.compile(r"Break-Even Scratch Trades\s+:\s+(\d+)")
    win_rate_re = re.compile(r"Win Rate\s+:\s+([\d\.]+)%")
    gross_profit_re = re.compile(r"Gross Trading Profit\s+:\s+\+\s+([\d\.]+)\s+INR")
    gross_loss_re = re.compile(r"Gross Trading Loss\s+:\s+-\s+([\d\.]+)\s+INR")
    profit_factor_re = re.compile(r"Profit Factor\s+:\s+([\d\.]+)")
    expectancy_re = re.compile(r"Trade Expectancy\s+:\s+([+-]?[\d\.]+)\s+INR")
    max_intra_dd_re = re.compile(r"Max Intraday Drawdown\s+:\s+([\d\.]+)\s+INR")
    max_port_dd_re = re.compile(r"Max Portfolio Drawdown\s+:\s+([\d\.]+)\s+INR\s+\(([\d\.]+)%\)")
    active_sess_re = re.compile(r"Active Trading Sessions\s+:\s+(\d+)")
    sess_cls_re = re.compile(r"Profitable / Loss / Flat\s+:\s+(\d+)\s+/\s+(\d+)\s+/\s+(\d+)")

    lines = stdout_str.splitlines()
    for line in lines:
        m = session_re.match(line)
        if m:
            res.session_rows.append(
                SessionAuditRow(
                    day=int(m.group(1)),
                    date_ist=m.group(2),
                    trades=int(m.group(3)),
                    wins=int(m.group(4)),
                    losses=int(m.group(5)),
                    scratches=int(m.group(6)),
                    win_rate_pct=float(m.group(7)),
                    gross_pnl=float(m.group(8)),
                    friction=float(m.group(9)),
                    net_pnl=float(m.group(10)),
                    intra_dd=float(m.group(11)),
                    ending_equity=float(m.group(12)),
                    kill_switch=m.group(13),
                )
            )
            continue

        m_ttl = ttl_re.search(line)
        if m_ttl:
            res.session_total = {
                "trades": int(m_ttl.group(1)),
                "wins": int(m_ttl.group(2)),
                "losses": int(m_ttl.group(3)),
                "scratches": int(m_ttl.group(4)),
                "win_rate_pct": float(m_ttl.group(5)),
                "gross_pnl": float(m_ttl.group(6)),
                "friction": float(m_ttl.group(7)),
                "net_pnl": float(m_ttl.group(8)),
                "max_intra_dd": float(m_ttl.group(9)),
                "ending_equity": float(m_ttl.group(10)),
                "kill_switch_triggers": int(m_ttl.group(11)),
            }
            continue

        m_ks = ks_re.match(line)
        if m_ks:
            res.kill_switch_rows.append(
                KillSwitchRow(
                    day=int(m_ks.group(1)),
                    date_ist=m_ks.group(2),
                    trades=int(m_ks.group(3)),
                    starting_equity=float(m_ks.group(4)),
                    peak_equity=float(m_ks.group(5)),
                    trough_equity=float(m_ks.group(6)),
                    max_intra_dd=float(m_ks.group(7)),
                    max_intra_dd_pct=float(m_ks.group(8)),
                    status=m_ks.group(9),
                )
            )
            continue

        m_eq = eq_re.match(line)
        if m_eq:
            res.equity_curve_rows.append(
                EquityCurveRow(
                    day=int(m_eq.group(1)),
                    date_ist=m_eq.group(2),
                    trades=int(m_eq.group(3)),
                    closing_equity=float(m_eq.group(4)),
                    daily_return_pct=float(m_eq.group(5)),
                    cum_return_pct=float(m_eq.group(6)),
                    hwm=float(m_eq.group(7)),
                    drawdown_from_hwm=float(m_eq.group(8)),
                    drawdown_pct=float(m_eq.group(9)),
                )
            )
            continue

        m_stt = stt_re.search(line)
        if m_stt:
            res.statutory_charges["stt"] = float(m_stt.group(1))
            res.statutory_charges["stt_share"] = float(m_stt.group(2))
        m_nse = nse_re.search(line)
        if m_nse:
            res.statutory_charges["nse_turnover"] = float(m_nse.group(1))
            res.statutory_charges["nse_share"] = float(m_nse.group(2))
        m_gst = gst_re.search(line)
        if m_gst:
            res.statutory_charges["gst"] = float(m_gst.group(1))
            res.statutory_charges["gst_share"] = float(m_gst.group(2))
        m_stamp = stamp_re.search(line)
        if m_stamp:
            res.statutory_charges["stamp_duty"] = float(m_stamp.group(1))
            res.statutory_charges["stamp_share"] = float(m_stamp.group(2))
        m_sebi = sebi_re.search(line)
        if m_sebi:
            res.statutory_charges["sebi_turnover"] = float(m_sebi.group(1))
            res.statutory_charges["sebi_share"] = float(m_sebi.group(2))
        m_tot_f = tot_fric_re.search(line)
        if m_tot_f:
            res.statutory_charges["total_friction"] = float(m_tot_f.group(1))
        m_trn = turnover_re.search(line)
        if m_trn:
            res.statutory_charges["total_turnover"] = float(m_trn.group(1))
        m_ratio = fric_ratio_re.search(line)
        if m_ratio:
            res.statutory_charges["friction_drag_ratio"] = float(m_ratio.group(1))

        # Metrics
        m_tt = tot_trades_re.search(line)
        if m_tt: res.statistical_metrics["total_trades"] = int(m_tt.group(1))
        m_wt = win_trades_re.search(line)
        if m_wt: res.statistical_metrics["winning_trades"] = int(m_wt.group(1))
        m_lt = loss_trades_re.search(line)
        if m_lt: res.statistical_metrics["losing_trades"] = int(m_lt.group(1))
        m_st = scratch_trades_re.search(line)
        if m_st: res.statistical_metrics["scratch_trades"] = int(m_st.group(1))
        m_wr = win_rate_re.search(line)
        if m_wr: res.statistical_metrics["win_rate_pct"] = float(m_wr.group(1))
        m_gp = gross_profit_re.search(line)
        if m_gp: res.statistical_metrics["gross_profit"] = float(m_gp.group(1))
        m_gl = gross_loss_re.search(line)
        if m_gl: res.statistical_metrics["gross_loss"] = float(m_gl.group(1))
        m_pf = profit_factor_re.search(line)
        if m_pf: res.statistical_metrics["profit_factor"] = float(m_pf.group(1))
        m_ex = expectancy_re.search(line)
        if m_ex: res.statistical_metrics["expectancy"] = float(m_ex.group(1))
        m_mid = max_intra_dd_re.search(line)
        if m_mid: res.statistical_metrics["max_intraday_dd"] = float(m_mid.group(1))
        m_mpd = max_port_dd_re.search(line)
        if m_mpd:
            res.statistical_metrics["max_portfolio_dd"] = float(m_mpd.group(1))
            res.statistical_metrics["max_portfolio_dd_pct"] = float(m_mpd.group(2))
        m_as = active_sess_re.search(line)
        if m_as: res.statistical_metrics["active_sessions"] = int(m_as.group(1))
        m_sc = sess_cls_re.search(line)
        if m_sc:
            res.statistical_metrics["profitable_sessions"] = int(m_sc.group(1))
            res.statistical_metrics["loss_sessions"] = int(m_sc.group(2))
            res.statistical_metrics["flat_sessions"] = int(m_sc.group(3))

    # Parse log lines for events
    buy_re = re.compile(r"FILLED BUY:\s+(\w+)\s+Qty=(\d+)\s+@\s+([\d\.]+)\s+INR\s+\|\s+SL=([\d\.]+)\s+\|\s+TP=([\d\.]+)")
    sell_re = re.compile(
        r"FILLED SELL:\s+(\w+)\s+Qty=(\d+)\s+@\s+([\d\.]+)\s+INR\s+\|\s+Gross PnL:\s+([+-]?[\d\.]+)\s+INR\s+\|\s+Taxes:\s+-([\d\.]+)\s+INR\s+\|\s+Net PnL:\s+([+-]?[\d\.]+)\s+INR\s+\|\s+MAE:\s+([\d\.]+)\s+INR\s+\|\s+MFE:\s+([\d\.]+)\s+INR\s+\|\s+Balance:\s+([\d\.]+)\s+INR"
    )
    roll_re = re.compile(r"SESSION ROLLOVER: Transitioning to IST Day (\d+) \| Carried Portfolio Equity: ([\d\.]+) INR")
    rej_re = re.compile(r"Order rejected by RiskManager:\s+(.+)")
    veto_re = re.compile(r"WEBHOOK VETOED \[(\w+)\]:\s+(.+)")

    for log in log_lines:
        m_b = buy_re.search(log)
        if m_b:
            res.trades.append(
                TradeEvent(
                    side="BUY",
                    symbol=m_b.group(1),
                    quantity=int(m_b.group(2)),
                    price=float(m_b.group(3)),
                    sl=float(m_b.group(4)),
                    tp=float(m_b.group(5)),
                )
            )
            continue
        m_s = sell_re.search(log)
        if m_s:
            res.trades.append(
                TradeEvent(
                    side="SELL",
                    symbol=m_s.group(1),
                    quantity=int(m_s.group(2)),
                    price=float(m_s.group(3)),
                    gross_pnl=float(m_s.group(4)),
                    taxes=float(m_s.group(5)),
                    net_pnl=float(m_s.group(6)),
                    mae=float(m_s.group(7)),
                    mfe=float(m_s.group(8)),
                    balance=float(m_s.group(9)),
                )
            )
            continue
        m_r = roll_re.search(log)
        if m_r:
            res.rollover_events.append((int(m_r.group(1)), float(m_r.group(2))))
            continue
        m_rej = rej_re.search(log)
        if m_rej:
            res.rejections.append(m_rej.group(1))
            continue
        m_v = veto_re.search(log)
        if m_v:
            res.vetoes.append(f"{m_v.group(1)}: {m_v.group(2)}")
            continue

    return res


def run_engine(
    csv_path: str,
    config_dir: Optional[str] = None,
    webhook: Optional[str] = None,
    extra_args: Optional[List[str]] = None,
    cwd: Optional[Union[Path, str]] = None,
    preserve_state: bool = False,
    initial_state: Optional[Union[dict, str, Path]] = None,
    timeout_s: float = 60.0,
) -> EngineRunResult:
    """Execute the DeltaUno binary inside an isolated temporary directory."""
    engine_bin = find_engine_executable()

    # Resolve CSV path: if relative and exists in REPO_ROOT, make absolute
    csv_p = Path(csv_path)
    if not csv_p.is_absolute():
        repo_candidate = REPO_ROOT / csv_p
        if repo_candidate.exists():
            csv_p = repo_candidate.resolve()

    # Determine execution working directory
    is_temp = False
    temp_obj = None
    if cwd is not None:
        work_dir = Path(cwd).resolve()
    else:
        temp_obj = tempfile.TemporaryDirectory(prefix="deltauno_e2e_")
        work_dir = Path(temp_obj.name).resolve()
        is_temp = True

    try:
        # 1. Provide config directory inside work_dir if not explicitly specified
        dest_config = work_dir / "config"
        if not dest_config.exists() and (REPO_ROOT / "config").exists():
            shutil.copytree(REPO_ROOT / "config", dest_config)

        # 2. Inject initial state journal if requested for crash-recovery testing
        state_file = work_dir / "deltauno_state.json"
        if initial_state is not None:
            if isinstance(initial_state, dict):
                with open(state_file, "w", encoding="utf-8") as f:
                    json.dump(initial_state, f)
            elif isinstance(initial_state, str):
                with open(state_file, "w", encoding="utf-8") as f:
                    f.write(initial_state)
            elif isinstance(initial_state, Path):
                shutil.copyfile(initial_state, state_file)
        elif not preserve_state and state_file.exists():
            safe_unlink(state_file)

        # 3. Clean prior log inside work_dir
        log_file = work_dir / "deltauno.log"
        if log_file.exists():
            safe_unlink(log_file)

        # 4. Construct CLI command
        cmd = [str(engine_bin), "--csv", str(csv_p)]
        if config_dir:
            cmd.extend(["--config-dir", str(config_dir)])
        if webhook:
            cmd.extend(["--webhook", webhook])
        if extra_args:
            cmd.extend(extra_args)

        # 5. Execute subprocess
        proc = subprocess.run(
            cmd,
            cwd=work_dir,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_s,
        )

        # 6. Read log lines with retry loop for Windows handle release
        log_lines = []
        if log_file.exists():
            for attempt in range(5):
                try:
                    with open(log_file, "r", encoding="utf-8", errors="replace") as f:
                        log_lines = [line.strip() for line in f]
                    break
                except OSError:
                    time.sleep(0.02)

        # 7. Harvest state journal before directory cleanup
        state_raw = None
        state_parsed = None
        if state_file.exists():
            for attempt in range(5):
                try:
                    with open(state_file, "r", encoding="utf-8", errors="replace") as f:
                        state_raw = f.read()
                    state_parsed = json.loads(state_raw)
                    break
                except (OSError, json.JSONDecodeError):
                    time.sleep(0.02)

        parsed = parse_engine_output(proc.stdout, log_lines)
        parsed.returncode = proc.returncode
        parsed.stderr = proc.stderr
        parsed.state_journal = state_parsed
        parsed.state_journal_raw = state_raw
        return parsed

    finally:
        if is_temp and temp_obj is not None:
            safe_cleanup_tempdir(temp_obj)


# ==============================================================================
# GLOBAL MEMOIZED CACHE FOR FULL 30-DAY REPLAY
# ==============================================================================

_CACHED_30DAY_RUN: Optional[EngineRunResult] = None

def get_cached_30day_replay() -> EngineRunResult:
    """Return memoized results of the full 30-day historical replay."""
    global _CACHED_30DAY_RUN
    if _CACHED_30DAY_RUN is None:
        if not DATA_30DAY.exists():
            raise FileNotFoundError(f"30-day dataset missing: {DATA_30DAY}")
        _CACHED_30DAY_RUN = run_engine(str(DATA_30DAY))
    return _CACHED_30DAY_RUN


# ==============================================================================
# SYNTHETIC CSV GENERATION HELPERS
# ==============================================================================

def write_tick_csv(ticks: List[Dict[str, Any]], target_file: Path):
    """Write list of tick dictionaries into standard DeltaUno CSV format."""
    header = "#timestamp_ns,token,symbol,last_price_inr,last_qty,volume,bid0_p,bid0_q,ask0_p,ask0_q,upper_c,lower_c\n"
    with open(target_file, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
        for t in ticks:
            line = (
                f"{t['timestamp_ns']},"
                f"{t['token']},"
                f"{t['symbol']},"
                f"{t['last_price_inr']:.2f},"
                f"{t['last_qty']},"
                f"{t['volume']},"
                f"{t['bid0_p']:.2f},"
                f"{t['bid0_q']},"
                f"{t['ask0_p']:.2f},"
                f"{t['ask0_q']},"
                f"{t['upper_c']:.2f},"
                f"{t['lower_c']:.2f}\n"
            )
            f.write(line)
