#!/usr/bin/env bash
# Vehicle (normal driving) run script.
#
# Default: run the gatekeeper on this Jetson as a local SIM (virtual UART, PTY) and
#   validate the whole loop with it. It uses exactly the same byte protocol as the
#   real physical UART (USB-TTL <-> STM32 D0/D1), so once the firmware is on the
#   STM32 just add --real.
# Real-board mode: --real connects directly to /dev/ttyUSB0 (the STM32 must already
#   be flashed per docs/STM32_PORT.md to get responses).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

DURATION="${1:-15}"
USE_REAL=false
if [[ "${2:-}" == "--real" ]]; then
    USE_REAL=true
fi

if [[ "$USE_REAL" == true ]]; then
    echo "[run_vehicle] using real STM32 board at $REAL_UART_PORT"
    python3 -m jetson.ecu.main_ecu --transport uart --uart-port "$REAL_UART_PORT" --duration "$DURATION"
else
    trap stop_gatekeeper_local EXIT
    start_gatekeeper_local
    python3 -m jetson.ecu.main_ecu --transport uart --uart-port "$SIM_LINK_PATH" --duration "$DURATION"
fi
