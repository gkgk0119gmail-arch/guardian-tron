#!/usr/bin/env bash
# Threat 3 demo: OTA/firmware tampering vs Root of Trust (RoT). No UART link needed
# (it covers the pre-boot signature verification stage).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

build_gatekeeper_if_needed
python3 -m jetson.threats.threat3_ota_tamper --firmware "$GATEKEEPER_SIM_BIN" "$@"
