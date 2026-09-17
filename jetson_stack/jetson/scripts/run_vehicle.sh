#!/usr/bin/env bash
# 차량(정상 주행) 구동 스크립트.
#
# 기본: 게이트키퍼를 이 젯슨에서 로컬 SIM(가상 UART, PTY)으로 띄우고 그걸로
#   전체 루프를 검증한다. 실제 물리 UART(USB-TTL <-> STM32 D0/D1)와 완전히
#   동일한 바이트 프로토콜을 쓰므로, 펌웨어만 STM32에 올라가면 --real 옵션만
#   추가하면 된다.
# 실기판 모드: --real 을 주면 /dev/ttyUSB0로 직접 연결한다 (STM32에
#   docs/STM32_PORT.md 절차로 펌웨어가 이미 플래시되어 있어야 응답이 온다).
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
