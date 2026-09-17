# STM32N6570-DK 실기판 이식 — 남은 작업

> **2026-09-13 갱신**: 실제 배선을 최종 확인 — USB-to-TTL 어댑터로 젯슨과
> STM32N6570-DK의 Arduino 커넥터 **D0/D1(USART2, PF6/PD5)**을 직결했다
> (물리 연결/전압/드라이버까지 검증 완료, 아래 "확인된 것" 참고). 따라서
> **UART가 실제 채택 경로**이고, `gatekeeper/stm32/gatekeeper_uart_bridge.c`
> 가 실기판에 올라갈 브리지 코드다. 이전에 작성한 이더넷/lwIP 경로
> (`gatekeeper_lwip_udp.c`)는 Hokuyo 라이다 등 네트워크 장비와 별개로 여전히
> 유효하지만, STM32 게이트키퍼 자체의 1차 연결 방식은 UART로 확정한다.

`gatekeeper/src/{protocol,safety_envelope,gatekeeper_core}.c` 는 HAL/CMSIS를
전혀 참조하지 않는 순수 C(C11)라서 **수정 없이 그대로** 실기판 프로젝트에
들어간다. `gatekeeper/sim/main_sim.c`(Linux PTY)가 하던 "바이트 스트림 →
STX 동기화 → 15바이트 프레임 조립 → gk_core_process 호출 → 응답 송신" 역할을
실기판에서는 `gatekeeper/stm32/gatekeeper_uart_bridge.c`(HAL UART 인터럽트
+ DWT 사이클 카운터)가 대신한다 — 로직은 완전히 동일, 전송 계층만 다르다.

## 확인된 것 (물리 배선)

| 항목 | 값 |
|---|---|
| 젯슨 | USB-TTL 어댑터(PL2303 칩) → `/dev/ttyUSB0` |
| STM32 | Arduino 커넥터 D0(=USART2_RX, PF6) / D1(=USART2_TX, PD5) |
| 보레이트 | 115200 8N1 |
| 커널 드라이버 | 이 젯슨 커널(5.15.148-tegra)에 `pl2303.ko`가 기본 포함 안 되어 있어서, Linux v5.15 태그 소스로 직접 빌드함 (`toolchain/pl2303-build/`) — 재부팅하면 `insmod`부터 다시 필요 (아래 참고) |
| VESC | fw 6.6, v_in=15.1V 확인됨 (테스트 당시 `/dev/ttyACM0`로 젯슨에 임시 벤치 연결). **프로덕션 배선은 STM32 전용** — 어느 USART 쓸지는 미확정 (아래 4b 참고) |
| 젯슨용 라이다 | Hokuyo UST-10LX, 이더넷 `192.168.0.10:10940`, SCIP2.0 — `jetson/ecu/lidar.py`로 검증됨 |
| STM32용 센서 | 라이다 아님 — **STM32N6570-DK 온보드 카메라 + Neural-ART NPU** 사용 (`local_safety_monitor.c`, 아직 스텁) |

재부팅 후 매번 실행해야 하는 것:
```bash
sudo modprobe usbserial
sudo insmod /home/orin/미래모빌리티/toolchain/pl2303-build/pl2303.ko
sudo udevadm trigger
# ls /dev/ttyUSB0 로 확인
```
(영구적으로 자동 로드되게 하려면 `/etc/modules-load.d/`와
`/lib/modules/$(uname -r)/kernel/drivers/usb/serial/`에 등록하는 방법도
있음 — 필요하면 별도로 설정)

## 왜 지금 당장 실기판 빌드가 안 되는가

STM32N6는 최근 출시 칩이라:
- 레지스터/클럭/USART 초기화 코드(CMSIS-Device 헤더, 스타트업 어셈블리,
  링커 스크립트)는 ST가 STM32CubeMX/STM32CubeIDE로 코드생성하는 것이
  표준 경로이며, 임의로 작성하면 안전 크리티컬 보드에서 오동작 위험이 있다.
- 보안 부팅(FSBL 서명 헤더 부착)은 **STM32CubeProgrammer**가 필요하다.
- 둘 다 ST가 x86_64만 배포 (ARM64 미지원) → 이 젯슨(aarch64)에는 설치 불가,
  **x86_64 데스크탑에서 진행**.

## 진행 순서 (데스크탑 PC, STM32CubeIDE 설치 완료 상태 기준)

1. **STM32CubeMX로 프로젝트 생성**: File → New → STM32 Project → 보드
   `STM32N6570-DK` 선택.
   - Connectivity → **USART2** 활성화, Mode: Asynchronous
   - Parameter Settings: Baud Rate `115200`, Word Length 8 Bits, Parity
     None, Stop Bits 1 (기본값 그대로면 이미 8N1)
   - NVIC 탭에서 USART2 global interrupt **Enabled** 확인 (HAL_UART_Receive_IT
     쓰려면 필수)
   - Clock Configuration은 CubeMX 기본 추천값 사용
   - Generate Code
2. 생성된 프로젝트의 `Core/Src/` `Core/Inc/` 옆에 아래 14개 파일을 그대로
   추가 (수정 없이 그대로 컴파일되어야 함 — `gcc -fsyntax-only`로 가짜 HAL
   헤더 대상 문법 검증은 이미 통과함, 실제 CubeIDE 빌드는 아직 안 해봄):
   - `gatekeeper/src/protocol.c`, `safety_envelope.c`, `gatekeeper_core.c`
   - `gatekeeper/include/protocol.h`, `safety_envelope.h`, `gatekeeper_core.h`
   - `gatekeeper/stm32/gatekeeper_uart_bridge.c`, `gatekeeper_uart_bridge.h`
   - `gatekeeper/stm32/dwt_timing.c`, `dwt_timing.h`
   - `gatekeeper/stm32/vesc_uart.c`, `vesc_uart.h`
   - `gatekeeper/stm32/local_safety_monitor.c`, `local_safety_monitor.h`
     (STM32 온보드 카메라 + NPU 기반 충돌확률 스텁 — 항상 "안전" 반환,
     실제 X-CUBE-AI 모델 연동되면 `local_safety_poll()` 내부만 교체)
3. `Core/Src/main.c`에서 `MX_USART2_UART_Init();` 호출 바로 다음 줄에 추가
   (USER CODE 블록 안에 넣어야 CubeMX 재생성 시 안 지워짐):
   ```c
   #include "gatekeeper_uart_bridge.h"
   ...
   MX_USART2_UART_Init();
   gatekeeper_uart_init(&huart2);
   ```
4. `Core/Src/stm32n6xx_it.c` (또는 main.c의 USER CODE 콜백 영역)에
   `HAL_UART_RxCpltCallback`이 아직 없다면 추가하고 브리지로 위임:
   ```c
   void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
       gatekeeper_uart_rx_cplt_callback(huart);
   }
   ```
   (CubeMX가 이미 이 함수의 weak 기본 구현을 어딘가에 만들어뒀을 수 있음 —
   그 경우 거기에 위 한 줄만 추가)
4b. **VESC 연결 (별도 USART, 가정값 — CubeMX에서 확정 필요)**: VESC는
   젯슨이 아니라 STM32에만 연결한다 (설계 결정 — 게이트키퍼가 최종 구동
   권한을 가지려면 Jetson이 액추에이터를 직접 건드릴 수 있는 경로가 아예
   없어야 함). Arduino D0/D1은 이미 USART2로 Jetson 링크에 쓰고 있고,
   STMod+ 커넥터도 전기적으로 동일한 USART2라서 VESC용으로 못 쓴다.

   **지금은 `USART3`을 임시 가정값으로 코드를 완성해뒀다** (실제로 이
   보드에서 USART3이 어느 핀에 나오는지는 미확인 — CubeMX Pinout 화면에서
   D2/D3/D6/D7 등을 클릭해 Alternate Function 목록을 확인해야 확정된다).
   CubeMX에서 확인되면 프로젝트에서 USART3을 활성화하고, 생성된 핸들
   이름이 `huart3`이 아니면 아래 `main.c` 스니펫의 이름만 맞춰 바꾸면 끝
   — 로직 코드(`vesc_uart.c`)는 수정할 필요 없다.
4c. **`main.c` 통합 (전체 초기화 블록, 그대로 복붙 가능)**: CubeMX가 생성한
   `MX_USART2_UART_Init();` / `MX_USART3_UART_Init();` 호출 다음, `while(1)`
   루프 시작 전 USER CODE 블록에 아래를 추가:
   ```c
   #include "gatekeeper_uart_bridge.h"
   #include "vesc_uart.h"
   /* USER CODE BEGIN 2 */
   vesc_uart_init(&huart3);        /* VESC 전용 — Jetson 링크(huart2)와 다른 UART */
   gatekeeper_uart_init(&huart2);  /* Jetson 링크, 여기서 DWT/게이트키퍼 상태 초기화 */
   /* USER CODE END 2 */
   ```
   그리고 인터럽트 콜백 (CubeMX가 만든 weak 함수 자리에 또는
   `stm32n6xx_it.c`에 새로 추가):
   ```c
   void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
       gatekeeper_uart_rx_cplt_callback(huart);
   }
   ```
   `gatekeeper_periodic_safety_check()`는 50Hz 주기 타이머(TIM 인터럽트)에
   연결해야 한다 — CubeMX에서 TIMx를 50Hz로 설정하고 Enabled interrupt 켠 뒤:
   ```c
   void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
       if (htim->Instance == TIM6 /* CubeMX가 준 실제 타이머로 교체 */) {
           gatekeeper_periodic_safety_check();
       }
   }
   ```
   (타이머 번호도 미확정 — 아무 여유 TIM이나 CubeMX에서 골라 50Hz로 설정하면 됨.)

5. μT-Kernel 3.0 이식은 T-Engine Forum의 공식 BSP2가 필요하다(라이선스
   동의 필요, 자동 다운로드 불가). 이식되면 3~4번의 초기화/콜백을
   μT-Kernel 태스크(100Hz 주기)로 감싸면 된다 — `gk_core_process()` 자체는
   OS 유무와 무관하게 그대로 재사용.
6. Build → Run (ST-Link USB-C, 데스크탑↔보드). 여기서 실제 플래시됨.
7. STM32CubeProgrammer로 서명 부팅 파이프라인 구성 (FSBL 서명 헤더는
   `jetson/threats/threat3_ota_tamper.py`의 `sign_image()`와 같은 개념이나,
   실제 STM32N6 보안부팅 헤더 포맷은 ST 문서 규격을 따라야 한다).
8. 플래시 완료 후 USB-TTL 케이블은 그대로 둔 채(플래싱용 ST-Link USB만
   분리), 젯슨에서:
   ```bash
   ./jetson/scripts/run_vehicle.sh 15 --real
   ```

## 지금 검증된 것 / 아닌 것

- ✅ 안전 로직(안전포락선, veto, 리플레이 방어, CRC)은 `gatekeeper_sim`으로
  실제 실행·검증됨 — 실기판에 그대로 들어갈 코드가 이미 동작 중.
- ✅ UART 프레이밍 프로토콜은 PTY 기반이지만 진짜 바이트 스트림으로 실측됨.
- ✅ 젯슨↔STM32 물리 UART 배선 및 USB-TTL 드라이버 — 확인 완료.
- ✅ VESC 프로토콜 C 포팅(`vesc_uart.c`) — CRC16은 Python 버전과 바이트
  단위로 교차검증, `COMM_SET_SERVO_POS`(id 12) 인코딩은 공식
  vedderb/bldc 펌웨어 소스와 대조 확인. 가짜 HAL 헤더 대상 문법 검증 통과.
- ❌ STM32 쪽 실제 응답 — 아직 (CubeIDE 프로젝트는 있다고 하셨지만 이
  파일들이 아직 그 프로젝트에 추가/빌드되지 않음, 펌웨어 미탑재).
- ❌ VESC용 USART 번호 — `USART3`은 가정값, CubeMX로 실제 확정 필요.
- ❌ STM32 온보드 카메라 + NPU 충돌확률 파이프라인 — CubeMX 카메라/ISP
  설정과 X-CUBE-AI 모델 배포가 아직 없음. `local_safety_monitor.c`는
  항상 "장애물 없음"을 반환하는 스텁 상태로 남아있음 (그래도 나머지
  펌웨어는 이 스텁 상태로 완전히 컴파일·동작 가능).
- ❌ 실기판 위에서의 마이크로초 지연 실측 (DWT 카운터, 600MHz 클럭) — 아직.
- ❌ μT-Kernel 3.0 태스크 스케줄링 하의 동작 — 아직 (지금은 순수 HAL
  인터럽트/폴링 기반 베어메탈 구조; SafetyMonitor를 별도 태스크로 돌리는
  등 μT-Kernel의 실제 이점은 통합 후에나 나타남).
- ❌ 실제 보안 부팅 파이프라인 — 개념만 (Ed25519 데모), STM32N6 고유 헤더
  포맷 아님.
