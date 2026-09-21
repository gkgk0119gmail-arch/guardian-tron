#!/usr/bin/env python3
"""pc_host.py - stand-in for the Jetson on any PC (Windows/macOS/Linux) with a 3.3 V USB-TTL.

Lets the STM32N6570-DK be evaluated on a desk without the Jetson or the car: sends the
same 15-byte CMD frames the Jetson sends (100 Hz) to USART2 (Arduino D0/D1) and reads
the 12-byte VERDICT frames back.

    python3 pc_host.py --port /dev/ttyUSB0 --speed 0.5 --secs 30            # normal drive command
    python3 pc_host.py --port COM5 --scenario freeze                        # host stalls 1.5 s -> STM32 watchdog
    python3 pc_host.py --port COM5 --scenario unsafe                        # 40 deg steering -> VETO (clamped to 17.5 deg)
    python3 pc_host.py --port COM5 --scenario replay                        # old sequence numbers -> rejected
    python3 pc_host.py --port COM5 --scenario garbage                       # bad CRC / noise -> MALFORMED

Wiring: USB-TTL TXD -> D0 (PF6), RXD -> D1 (PD5), GND -> GND. 115200 8N1. Requires pyserial.
"""
import argparse
import os
import struct
import sys
import time

import serial

CMD_STX, VERDICT_STX, ETX = 0xAA, 0xBB, 0x55
NAMES = {0: 'APPROVED', 1: 'VETO', 2: 'MALFORMED'}


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def cmd(seq: int, steer_deg: float, speed: float) -> bytes:
    body = struct.pack('<Iff', seq & 0xFFFFFFFF, steer_deg, speed)
    return bytes([CMD_STX]) + body + bytes([crc8(body), ETX])


def run_scenario(port, scenario='normal', speed=0.5, steer=0.0, secs=20.0, rate=100.0, log=print):
    """Play the Jetson for `secs` seconds on `port` (a device name or an open serial.Serial).
    Returns the verdict counts, e.g. {'APPROVED': 1830, 'VETO': 190, 'MALFORMED': 0}."""
    s = serial.Serial(port, 115200, timeout=0) if isinstance(port, str) else port
    time.sleep(0.2)
    s.reset_input_buffer()
    # the gatekeeper rejects seq <= last seen (replay protection) and keeps that across
    # host restarts: seed from the wall clock so a new run always continues above it
    seq = int(time.time() * 1000) & 0x7FFFFFFF
    period = 1.0 / rate
    t0 = time.monotonic()
    nxt = t0
    sent = {}
    counts = {'APPROVED': 0, 'VETO': 0, 'MALFORMED': 0}
    lat = []
    rx = b''
    last_print = t0
    fault_at = t0 + secs / 2
    fault_done = False
    said = False
    log(f'[host] {getattr(s, "port", port)} scenario={scenario} speed={speed} m/s steer={steer} deg for {secs:.0f} s')

    while time.monotonic() - t0 < secs:
        now = time.monotonic()
        st = steer
        if not fault_done and now >= fault_at and scenario != 'normal':
            if scenario == 'freeze':
                log('[host] FAULT: host stalls for 1.5 s (no frames) -> expect STM32 "[fault] Jetson silent"')
                time.sleep(1.5)
                fault_done = True
                nxt = time.monotonic()
                continue
            if scenario == 'unsafe' and not said:
                said = True
                log('[host] FAULT: 2 s of 40 deg steering commands -> expect VETO (clamped to the 17.5 deg envelope)')
            if scenario == 'replay':
                log('[host] FAULT: replaying 50 old sequence numbers -> expect them to be rejected')
                for k in range(50):
                    s.write(cmd(seq - 1000 + k, 0.0, speed))
                    time.sleep(period)
                fault_done = True
            if scenario == 'garbage':
                log('[host] FAULT: 50 frames with a broken CRC + random noise -> expect MALFORMED / resync')
                for k in range(50):
                    f = bytearray(cmd(seq + k, 0.0, speed)); f[13] ^= 0x5A
                    s.write(bytes(f) + os.urandom(3))
                    time.sleep(period)
                seq += 50
                fault_done = True
        if scenario == 'unsafe' and fault_at <= now < fault_at + 2.0:
            st = 40.0
        elif scenario == 'unsafe' and now >= fault_at + 2.0:
            fault_done = True

        seq += 1
        sent[seq] = time.monotonic()
        s.write(cmd(seq, st, speed))

        rx += s.read(256)
        while len(rx) >= 12:
            i = rx.find(bytes([VERDICT_STX]))
            if i < 0:
                rx = b''
                break
            rx = rx[i:]
            if len(rx) < 12:
                break
            f, rx = rx[:12], rx[12:]
            if f[11] != ETX or crc8(f[1:10]) != f[10]:
                rx = f[1:] + rx
                continue
            vseq, verdict, lat_us = struct.unpack('<IBI', f[1:10])
            counts[NAMES.get(verdict, '?')] = counts.get(NAMES.get(verdict, '?'), 0) + 1
            if vseq in sent:
                lat.append((time.monotonic() - sent.pop(vseq)) * 1e3)
        if now - last_print >= 1.0:
            last_print = now
            l = sorted(lat)
            p99 = l[int(len(l) * 0.99)] if l else 0
            log(f'[host] t={now - t0:5.1f}s sent={seq & 0xFFFF:5d} ' + ' '.join(f'{k}={v}' for k, v in counts.items())
                + (f' | round trip avg {sum(l) / len(l):.2f} ms p99 {p99:.2f} ms' if l else ' | no verdicts yet'))
            lat.clear()
        nxt += period
        time.sleep(max(0.0, nxt - time.monotonic()))

    log('[host] done: ' + ' '.join(f'{k}={v}' for k, v in counts.items()))
    if isinstance(port, str):
        s.close()
    return counts


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--speed', type=float, default=0.5, help='target speed [m/s] (STM32 caps at 2.0)')
    ap.add_argument('--steer', type=float, default=0.0, help='steering [deg]')
    ap.add_argument('--secs', type=float, default=20.0)
    ap.add_argument('--rate', type=float, default=100.0)
    ap.add_argument('--scenario', choices=['normal', 'freeze', 'unsafe', 'replay', 'garbage'], default='normal')
    a = ap.parse_args()
    run_scenario(a.port, a.scenario, a.speed, a.steer, a.secs, a.rate)
    return 0


if __name__ == '__main__':
    sys.exit(main())
