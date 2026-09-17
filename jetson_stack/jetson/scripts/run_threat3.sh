#!/usr/bin/env bash
# 위협③ OTA/펌웨어 침해 vs 신뢰루트(RoT) 시연. UART 링크가 필요 없다
# (부팅 전 서명 검증 단계를 다루므로).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

build_gatekeeper_if_needed
python3 -m jetson.threats.threat3_ota_tamper --firmware "$GATEKEEPER_SIM_BIN" "$@"
