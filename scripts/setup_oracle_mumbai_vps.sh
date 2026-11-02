#!/usr/bin/env bash
# ==============================================================================
# DeltaUno v3.3.0 — Oracle Cloud Mumbai (ap-mumbai-1) Always-Free VPS Provisioner
# ==============================================================================
# Configures an Ubuntu 22.04 / 24.04 VM (x86_64 or ARM64 Ampere A1) in Mumbai:
#   1. Sets system clock to IST (Asia/Kolkata) + chrony NTP sync
#   2. Installs C++20 toolchain (g++, cmake, ninja-build) & Python 3 venv
#   3. Compiles DeltaUno C++20 engine in Release mode (-O3)
#   4. Creates isolated Python virtualenv (.venv) with websocket-client & pyotp
#   5. Registers systemd service & Mon-Fri 09:10:00 IST timer (deltauno-live.timer)
# ==============================================================================
set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/CA-BijiteshKanrar/DeltaUno.git}"
INSTALL_DIR="${INSTALL_DIR:-$HOME/DeltaUno}"
SERVICE_USER="$(whoami)"

echo "========================================================================"
echo "  DeltaUno v3.3.0 — Oracle Cloud Mumbai (ap-mumbai-1) Setup"
echo "========================================================================"

echo "[1/6] Setting timezone to Asia/Kolkata (IST) and installing packages..."
sudo timedatectl set-timezone Asia/Kolkata
sudo apt-get update -y
sudo apt-get install -y \
    build-essential \
    g++ \
    cmake \
    git \
    curl \
    python3 \
    python3-venv \
    python3-pip \
    chrony

sudo systemctl enable --now chrony

echo "[2/6] Cloning / updating DeltaUno repository at ${INSTALL_DIR}..."
if [ ! -d "${INSTALL_DIR}/.git" ]; then
    git clone "${REPO_URL}" "${INSTALL_DIR}"
else
    git -C "${INSTALL_DIR}" fetch --all --tags
    git -C "${INSTALL_DIR}" pull --ff-only origin main
fi

cd "${INSTALL_DIR}"

echo "[3/6] Verifying v3.2.0 parameter freeze..."
git diff --exit-code v3.2.0 -- \
    include/deltauno/strategy/ \
    include/deltauno/risk/ \
    config/risk_limits.json \
    config/instruments.json \
    config/calendar_events.json
echo "  [OK] Strategy & risk files match frozen v3.2.0 baseline."

echo "[4/6] Building C++20 Engine (Release -O3) and running 9-suite CTest verification..."
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure

echo "[5/6] Setting up isolated Python virtualenv (.venv)..."
python3 -m venv "${INSTALL_DIR}/.venv"
"${INSTALL_DIR}/.venv/bin/pip" install --upgrade pip
"${INSTALL_DIR}/.venv/bin/pip" install websocket-client pyotp

if [ ! -f "${INSTALL_DIR}/.env" ]; then
    cat << 'EOF' > "${INSTALL_DIR}/.env"
# ==============================================================================
# DeltaUno Broker & Telemetry Credentials (chmod 600)
# ==============================================================================
# Primary L2 WebSocket Streamer (AngelOne SmartStream v2 Binary L2)
ANGELONE_API_KEY=
ANGELONE_CLIENT_CODE=
ANGELONE_PASSWORD=
ANGELONE_TOTP_SECRET=

# Failover L2 Provider (Upstox API v2)
UPSTOX_ACCESS_TOKEN=

# Optional: Kotak Neo Trade API v2 (₹0 Brokerage API — whitelist this VM's Static IP)
KOTAK_CONSUMER_KEY=
KOTAK_MOBILE_NUMBER=
KOTAK_UCC=
KOTAK_TOTP_SECRET=
KOTAK_MPIN=

# Google Sheets Live Bridge Webhook URL
GOOGLE_SHEETS_WEBHOOK_URL=
EOF
    chmod 600 "${INSTALL_DIR}/.env"
    echo "  [CREATED] ${INSTALL_DIR}/.env template (fill in your credentials)."
fi

echo "[6/6] Installing systemd service & Mon-Fri 09:10 IST timer..."
sudo tee /etc/systemd/system/deltauno-live.service > /dev/null << EOF
[Unit]
Description=DeltaUno v3.3.0 Live NSE Paper Trading Session (WebSocket -> --stdin-stream)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${SERVICE_USER}
WorkingDirectory=${INSTALL_DIR}
EnvironmentFile=${INSTALL_DIR}/.env
ExecStart=${INSTALL_DIR}/.venv/bin/python ${INSTALL_DIR}/scripts/run_live_paper_trading.py --provider auto --mode both
Restart=on-failure
RestartSec=10
TimeoutStopSec=60
StandardOutput=append:${INSTALL_DIR}/live_run_logs/systemd_session.log
StandardError=append:${INSTALL_DIR}/live_run_logs/systemd_session.log

[Install]
WantedBy=multi-user.target
EOF

sudo tee /etc/systemd/system/deltauno-live.timer > /dev/null << 'EOF'
[Unit]
Description=Run DeltaUno NSE Live Session Mon-Fri at 09:10:00 IST

[Timer]
OnCalendar=Mon..Fri *-*-* 09:10:00 Asia/Kolkata
Persistent=true
AccuracySec=1s

[Install]
WantedBy=timers.target
EOF

mkdir -p "${INSTALL_DIR}/live_run_logs" "${INSTALL_DIR}/data"
sudo systemctl daemon-reload
sudo systemctl enable --now deltauno-live.timer

PUBLIC_IP="$(curl -s --max-time 5 https://ifconfig.me || echo 'unknown')"
echo "========================================================================"
echo "  [READY] DeltaUno Oracle Mumbai VPS Configured!"
echo "  Public Static IPv4 (Whitelist in Kotak Neo / AngelOne): ${PUBLIC_IP}"
echo "  Next Steps:"
echo "    1. Populate credentials: nano ${INSTALL_DIR}/.env"
echo "    2. Test connection:      ${INSTALL_DIR}/.venv/bin/python ${INSTALL_DIR}/scripts/run_live_paper_trading.py --provider auto --test-connection"
echo "    3. Check timer schedule: systemctl list-timers deltauno-live.timer"
echo "========================================================================"
