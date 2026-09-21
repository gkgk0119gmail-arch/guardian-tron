#!/usr/bin/env python3
"""Simple check of the USB-TTL <-> STM32 physical link.

Usage:
  python3 jetson/scripts/check_uart_link.py [port] [baud]
  Defaults: /dev/ttyUSB0, 115200

Normal (no firmware): nothing arrives during idle (0 bytes).
If repeating bytes keep arriving, it is a wiring/GND problem.
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
