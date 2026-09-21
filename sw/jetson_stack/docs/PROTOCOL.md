# Safety-Critical UART Link Protocol

Jetson(메인 ECU) → STM32(게이트키퍼) 방향의 제어 명령 프레임과, 게이트키퍼 →
Jetson 방향의 판정(verdict) 응답 프레임을 정의한다. UART는 스트림이므로
바이트 경계를 잡기 위해 STX/ETX와 CRC8을 둔다.

기본 설정: **115200 baud, 8N1**, 실제 배선 시 보드에 맞는 UART 포트/보레이트로
`config.h`(STM32측) / `--baud`(Jetson측) 를 함께 맞춰야 한다.

## CMD 프레임 (Jetson → 게이트키퍼), 15 bytes

| offset | size | field | type | 설명 |
|---|---|---|---|---|
| 0 | 1 | STX | u8 | 0xAA 고정 |
| 1 | 4 | seq | u32 LE | 송신 순번(모노토닉 증가) |
| 5 | 4 | steer_deg | f32 LE | 조향각 (도) |
| 9 | 4 | accel_mps2 | f32 LE | 종방향 가속 (m/s^2) |
| 13 | 1 | crc8 | u8 | offset 1..12 에 대한 CRC-8(poly 0x07) |
| 14 | 1 | ETX | u8 | 0x55 고정 |

## VERDICT 프레임 (게이트키퍼 → Jetson), 12 bytes

| offset | size | field | type | 설명 |
|---|---|---|---|---|
| 0 | 1 | STX2 | u8 | 0xBB 고정 |
| 1 | 4 | seq | u32 LE | 대응하는 CMD의 seq |
| 5 | 1 | verdict | u8 | 0=APPROVED, 1=VETO(포락선 위반), 2=MALFORMED(CRC/파싱 실패) |
| 6 | 4 | latency_us | u32 LE | 수신→판정 지연 (마이크로초) |
| 10 | 1 | crc8 | u8 | offset 1..9 CRC-8 |
| 11 | 1 | ETX2 | u8 | 0x55 고정 |

## 안전포락선 (safety_envelope.c 와 동일)

- 절댓값: `|steer_deg| <= 540`, `accel_mps2 ∈ [-10, 4]`
- 변화율(1 사이클, 100Hz 기준): `|Δsteer_deg| <= 10`, `|Δaccel_mps2| <= 1`
- 두 조건 중 하나라도 위반 → VETO, 직전 승인 명령(u_safe) 유지

## 위협 시나리오와 프레임 대응

- **위협① 적대적 인지공격(FGSM)**: 정상 CMD 프레임과 동일한 형식이지만
  perception이 만들어낸 위험한 steer/accel 값을 담음 → 게이트키퍼는 값 기반으로
  포락선 검증 (프레임 자체는 정상이므로 이게 핵심 검증 대상).
- **위협② CAN 인젝션/스푸핑 유사 공격**: UART 링크에 직접 임의/스푸핑된
  CMD 프레임을 주입 (Jetson 프로세스를 거치지 않고 소켓/시리얼에 raw 바이트 삽입).
  게이트키퍼는 시퀀스 이상·포락선 위반·CRC 실패를 이용해 방어.
- **위협③ OTA/펌웨어 침해**: 게이트키퍼 바이너리 자체를 변조해 재적재를 시도 →
  RoT(서명 검증, `firmware_sign.py` 참조)가 서명 불일치 시 로드 거부.
