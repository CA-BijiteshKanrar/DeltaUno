#!/usr/bin/env python3
"""
DeltaUno — Automated 09:05 AM IST GitHub Actions Dispatcher
===========================================================
Bypasses GitHub Actions' cron queue backlog by checking at 09:05 AM IST
whether today's trading session is already queued/running. If not, dispatches
`live_paper_trading.yml` via GitHub REST API (`workflow_dispatch`), which boots
the runner in < 2 seconds and waits cleanly for the 09:15:00 IST opening bell.
"""

import json
import subprocess
import sys
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone

TZ_IST = timezone(timedelta(hours=5, minutes=30))
REPO = "CA-BijiteshKanrar/DeltaUno"
WORKFLOW_FILE = "live_paper_trading.yml"


def get_github_token() -> str:
    p = subprocess.Popen(
        ["git", "credential", "fill"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, _ = p.communicate("protocol=https\nhost=github.com\n")
    creds = dict(line.split("=", 1) for line in stdout.strip().split("\n") if "=" in line)
    return creds.get("password", "")


def has_run_today(today_utc_prefix: str, today_ist_prefix: str) -> bool:
    url = f"https://api.github.com/repos/{REPO}/actions/runs?per_page=10"
    req = urllib.request.Request(url, headers={"User-Agent": "DeltaUno-AutoDispatcher"})
    with urllib.request.urlopen(req, timeout=10.0) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    for run in data.get("workflow_runs", []):
        status = run.get("status", "")
        created_at = run.get("created_at", "")
        if status in ("in_progress", "queued", "waiting", "requested"):
            print(f"[SKIP] Active run #{run['id']} is already {status} ({created_at}).")
            return True
        if created_at.startswith(today_utc_prefix) and run.get("conclusion") == "success":
            print(f"[SKIP] Run #{run['id']} already completed today ({created_at}).")
            return True
    return False


def dispatch_workflow(token: str, start_now: bool = False) -> int:
    url = f"https://api.github.com/repos/{REPO}/actions/workflows/{WORKFLOW_FILE}/dispatches"
    payload = json.dumps(
        {
            "ref": "main",
            "inputs": {
                "mode": "both",
                "provider": "auto",
                "start_now": "true" if start_now else "false",
                "max_runtime_minutes": "350",
            },
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=payload,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "User-Agent": "DeltaUno-AutoDispatcher",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10.0) as resp:
        print(f"[DISPATCHED] HTTP {resp.status} — DeltaUno session launched on GitHub Actions!")
    return 0


def main() -> int:
    now_ist = datetime.now(TZ_IST)
    now_utc = datetime.now(timezone.utc)
    if now_ist.weekday() >= 5 and "--force" not in sys.argv:
        print(f"[SKIP] Today is {now_ist.strftime('%A')} (NSE Weekend).")
        return 0

    if "--force" not in sys.argv and has_run_today(now_utc.strftime("%Y-%m-%d"), now_ist.strftime("%Y-%m-%d")):
        return 0

    token = get_github_token()
    if not token:
        print("[ERROR] Could not retrieve GitHub token from git credential store.", file=sys.stderr)
        return 1

    # If triggered before 09:15 IST, pass start_now=False so runner waits cleanly for 09:15:00 IST bell
    after_open = (now_ist.hour > 9) or (now_ist.hour == 9 and now_ist.minute >= 15)
    return dispatch_workflow(token, start_now=after_open)


if __name__ == "__main__":
    sys.exit(main())
