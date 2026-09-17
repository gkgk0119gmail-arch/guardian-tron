# SDV 안전 코프로세서 — 실행 가능한 스택

Guardian-TRON / SDV용 이종 안전 코프로세서 프로젝트의 실행 가능한
구현체. 젯슨(메인 AI ECU)과 STM32N6570-DK(안전 게이트키퍼)를 UART로 묶어,
정상 주행과 3가지 위협 시나리오를 실제로 돌려볼 수 있다.

## 지금 바로 되는 것 (SIM 모드)

STM32 실기판 없이도, 게이트키퍼의 **실제 안전 로직 C 코드**를 이 젯슨에서
네이티브로 컴파일해 가상 UART(PTY)로 띄우고, 젯슨 ECU/위협 스크립트가
거기에 진짜 UART 프레임을 주고받는다. `gatekeeper/src/*.c`는 실기판에
그대로 옮겨질 코드이므로(자세한 건 `docs/STM32_PORT.md`), 여기서 검증된
안전 판정 로직은 실기판에서도 동일하게 동작한다.

```bash
cd /home/orin/미래모빌리티

# 1) 정상 주행 15초
./jetson/scripts/run_vehicle.sh 15

# 2) 위협① FGSM 적대적 인지공격
./jetson/scripts/run_threat1.sh

# 3) 위협② UART 링크 스푸핑/리플레이/퍼징
./jetson/scripts/run_threat2.sh

# 4) 위협③ OTA/펌웨어 서명 위변조 vs 신뢰루트
./jetson/scripts/run_threat3.sh
```

## 실기판(STM32N6570-DK) 연동으로 넘어가려면

물리 배선은 이미 확인·완료됨: USB-TTL 어댑터로 젯슨(`/dev/ttyUSB0`) ↔
STM32N6570-DK Arduino D0/D1(USART2) 직결, 115200 8N1 (자세한 건
`docs/STM32_PORT.md`의 "확인된 것" 표 참고).

1. `docs/STM32_PORT.md` — STM32CubeIDE/CubeMX로 실기판 프로젝트 생성
   (USART2 활성화), `gatekeeper/src/*.c` + `gatekeeper/stm32/*.c`를 그대로
   이식, μT-Kernel 3.0 BSP2 통합.
2. 이식 후에는 각 스크립트에 `--real` 만 추가하면 SIM 게이트키퍼 대신
   실기판(`/dev/ttyUSB0`)으로 붙는다:
   ```bash
   ./jetson/scripts/run_vehicle.sh 15 --real
   ```
3. 재부팅 후에는 PL2303 커널 모듈을 다시 로드해야 `/dev/ttyUSB0`가 잡힌다
   (이 젯슨 커널에 기본 미포함이라 직접 빌드함, `toolchain/pl2303-build/`):
   ```bash
   sudo modprobe usbserial
   sudo insmod /home/orin/미래모빌리티/toolchain/pl2303-build/pl2303.ko
   sudo udevadm trigger
   ```

## 구조

```
gatekeeper/            게이트키퍼 핵심 로직 (하드웨어 독립적 C, 실기판에 그대로 이식)
  include/              protocol.h, safety_envelope.h, gatekeeper_core.h
  src/                   구현
  sim/main_sim.c         Linux PTY 위에서 돌리는 SIM 러너 (이식 대상 아님)
  sim/main_udp.c         UDP 버전 SIM 러너 (이더넷 경로용, 이식 대상 아님)
  stm32/                 실기판용 브리지 (CubeIDE 프로젝트에 그대로 추가)
    gatekeeper_uart_bridge.c/h   USART2 인터럽트 기반 브리지 (채택된 경로)
    gatekeeper_lwip_udp.c/h      lwIP 기반 브리지 (이더넷 대안 경로)
    dwt_timing.c/h                DWT 사이클 카운터 공용 유틸
    vesc_uart.c/h                 VESC 프로토콜 C 포팅 (STM32 전용 구동 경로)
    local_safety_monitor.c/h      STM32 온보드 카메라+NPU 충돌확률 스텁
  Makefile               make sim / make udp

jetson/
  ecu/                  메인 AI ECU (인지 추론 + UART/UDP 송신)
    perception.py        경량 신경망 (numpy, 실제 그래디언트 기반 FGSM 가능)
    protocol.py           gatekeeper/include/protocol.h 와 바이트 단위로 동일
    uart_link.py           pyserial 래퍼 (기본 전송 방식, /dev/ttyUSB0)
    udp_link.py             UDP 소켓 래퍼 (대안 경로)
    link_factory.py          CLI에서 두 전송 방식 선택하는 공용 헬퍼
    vesc.py                  VESC 모터 컨트롤러 UART 프로토콜 (실기기 검증됨)
    lidar.py                 Hokuyo UST-10LX 이더넷 드라이버 (실기기 검증됨)
    main_ecu.py               정상 주행 루프
  threats/
    threat1_fgsm.py        위협① 적대적 인지공격
    threat2_injection.py   위협② 링크 스푸핑/리플레이/퍼징
    threat3_ota_tamper.py  위협③ OTA 서명 위변조 vs RoT
  scripts/               run_*.sh 런처

toolchain/
  pl2303-build/          이 젯슨 커널에 빠진 PL2303 USB-TTL 드라이버 빌드본

docs/
  PROTOCOL.md            프레임 포맷, 안전포락선 정의
  WIRING.md              실배선 확인 사항 (초기 조사용, STM32_PORT.md가 최신)
  STM32_PORT.md          실기판 이식 로드맵, 확정된 배선, 현재 한계
```

## 안전 판정 로직 (논문 Ⅳ.3절, 식 2-4 구현)

- 절댓값 한계: `|steer_deg| <= 540`, `accel_mps2 ∈ [-10, 4]`
- 변화율 한계(100Hz 기준): `|Δsteer_deg| <= 10`, `|Δaccel_mps2| <= 1`
- 위반 시 직전 승인 명령(u_safe)을 유지하며 페일세이프 전이
- 추가: 시퀀스 리플레이/역행 탐지 시 포락선 통과 여부와 무관하게 veto
  (`gatekeeper/src/gatekeeper_core.c`) — 포락선만으로는 "정상값처럼
  보이는 재전송"을 못 막는다는 걸 위협②를 실제로 돌려보다가 발견해서
  추가한 방어.

## 알려진 한계 (정직하게)

- FGSM 공격은 급격한 스텝 변화(변화율 한계)는 확실히 막지만, 논문
  Ⅵ.2절이 스스로 인정하듯 **서서히 누적되는 미세 드리프트는 게이트키퍼
  단독으로 못 잡는다** — `run_threat1.sh` 를 돌려보면 공격 시작 시점은
  VETO되고 이후 비슷한 크기로 유지되면 APPROVED로 바뀌는 걸 볼 수 있다.
  이는 버그가 아니라 다층방어 철학상 인지 단계 강건성으로 보완해야 할
  부분이다.
- perception.py 는 논문의 실제 CUDA PilotNet이 아니라 numpy로 짠 2층
  신경망이다 (이 젯슨에 torch 미설치, Jetson 전용 wheel 설치는 별도
  작업 필요). `predict()`/`input_gradient()` 인터페이스만 맞추면 실제
  PilotNet으로 교체 가능.
- μT-Kernel 3.0, 실제 STM32N6 보안 부팅 헤더, NPU IDS는 아직 미이식 —
  `docs/STM32_PORT.md` 로드맵 참고.
