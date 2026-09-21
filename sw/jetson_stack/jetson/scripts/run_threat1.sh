#!/usr/bin/env bash
# Threat 1 demo: FGSM adversarial perception attack.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

USE_REAL=false
if [[ "${1:-}" == "--real" ]]; then
    USE_REAL=true
fi

if [[ "$USE_REAL" == true ]]; then
    python3 -m jetson.threats.threat1_fgsm --transport uart --uart-port "$REAL_UART_PORT"
else
    trap stop_gatekeeper_local EXIT
    start_gatekeeper_local
    python3 -m jetson.threats.threat1_fgsm --transport uart --uart-port "$SIM_LINK_PATH"
fi
