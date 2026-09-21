# 실기판 배선 (확인 필요)

현재 SIM 모드는 젯슨 안에서 가상 UART(PTY)로 게이트키퍼를 돌리기 때문에
물리 배선이 필요 없다. **실차 연동 시에는 아래를 실측·확정해야** 한다 —
잘못된 핀으로 배선하면 최악의 경우 게이트키퍼가 응답하지 않거나 보드가
손상될 수 있으므로 추측으로 진행하지 않는다.

## 확인해야 할 것

1. **어느 UART인가**: STM32N6570-DK는 Arduino 커넥터(CN 어딘가)와
   ST-Link 가상 COM 포트(USB) 양쪽에 UART가 나온다. ST-Link VCP를 쓰면
   USB-UART 브리지라서 배선이 필요 없고 `/dev/ttyACM0`처럼 잡힌다 (이번
   `lsusb`에서 `0483:5740 STMicroelectronics Virtual COM Port`가 이미
   보였다 — 이게 ST-Link VCP일 가능성이 높다). Arduino 헤더의 실제 GPIO
   UART(예: USART1 TX/RX)를 쓰려면 STM32N6570-DK 사용자 매뉴얼(UM3392)의
   커넥터 핀맵에서 정확한 핀 번호를 확인해야 한다.
2. **젯슨 쪽 UART**: Jetson Orin Nano의 40핀 헤더 UART(핀 8/10, `/dev/ttyTHS*`
   등) 를 쓸지, USB-UART(ST-Link VCP, `/dev/ttyACM0`)를 쓸지 결정. 후자가
   배선이 필요 없어 훨씬 간단하고 오류 여지가 적다 — 특별한 이유가 없다면
   **ST-Link USB VCP 경유를 권장**.
3. **보레이트**: 기본 115200 8N1로 맞춰뒀다 (`jetson/ecu/uart_link.py`
   `DEFAULT_BAUD`, `docs/PROTOCOL.md`). STM32 쪽 HAL UART 설정도 동일하게
   맞춰야 한다.
4. **공통 그라운드**: GPIO UART로 직결할 경우 젯슨과 STM32 보드의 GND를
   반드시 공통으로 연결해야 한다 (USB VCP 경유면 USB 케이블이 이미 처리).

## 확정되면

`jetson/scripts/run_vehicle.sh 15 --real-port /dev/ttyACM0` 처럼
`--real-port` 옵션으로 실제 장치 경로를 넘기면, SIM 게이트키퍼를 띄우지 않고
그 포트로 바로 붙는다 (STM32 쪽에는 `docs/STM32_PORT.md`의 게이트키퍼
펌웨어가 이미 플래시돼 있어야 함).
