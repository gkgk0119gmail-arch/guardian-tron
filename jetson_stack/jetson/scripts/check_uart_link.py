#!/usr/bin/env python3
"""USB-TTL <-> STM32 물리 연결 확인용 간단 스크립트.

사용법:
  python3 jetson/scripts/check_uart_link.py [포트] [보레이트]
  기본값: /dev/ttyUSB0, 115200

정상(펌웨어 없는 상태)이면: idle 구간에 아무것도 안 옴 (0바이트).
뭔가 계속 반복되는 바이트가 온다면 배선/GND 문제입니다.
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

print(f"[check] opening {port} @ {baud} baud...")
ser = serial.Serial(port, baudrate=baud, timeout=2)

print("[check] listening 3s WITHOUT sending anything (should be silent/empty if wiring is clean)...")
time.sleep(3)
idle = ser.read(500)
print(f"[check] idle bytes received: {len(idle)}")
if idle:
    print(f"[check]   sample: {idle[:40]!r}")
    if len(set(idle)) == 1:
        print(f"[check]   -> constant byte {idle[0]:#04x} repeated: almost certainly a wiring/GND/floating-line issue, not real data.")
    else:
        print("[check]   -> varying garbage: check baud rate matches on both ends, or still a wiring issue.")
else:
    print("[check]   -> silent, as expected with no firmware running yet. Wiring looks OK so far.")

print()
print("[check] sending a test string now...")
ser.reset_input_buffer()
ser.write(b"hello_stm32_test\n")
time.sleep(0.5)
resp = ser.read(500)
print(f"[check] response after send: {len(resp)} bytes")
if resp:
    print(f"[check]   sample: {resp[:40]!r}")
else:
    print("[check]   -> no response, expected (no firmware flashed on STM32 yet to echo/reply).")

ser.close()
print("\n[check] done.")
