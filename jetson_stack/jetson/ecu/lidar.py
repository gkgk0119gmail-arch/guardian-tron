"""Hokuyo UST-10LX LiDAR driver (SCIP 2.0 over TCP/Ethernet).

Confirmed against real hardware at 192.168.0.10:10940 on the same subnet as
the STM32 board:
  VV -> VEND: Hokuyo Automatic Co., Ltd.  PROD: UST-10LX  FIRM: 4.0.2-A
  PP -> DMIN:20 DMAX:30000 ARES:1440 AMIN:0 AMAX:1080 AFRT:540 SCAN:2400

NOTE: 192.168.0.10 is the same address earlier suggested as an example
STM32 static IP in docs/STM32_PORT.md -- pick a DIFFERENT address for the
board (e.g. 192.168.0.11) to avoid a collision on this subnet.

The one-shot "GD" command returns a non-zero status ("10") on this unit for
reasons not fully diagnosed -- possibly a firmware quirk or an SCIP2.0
nuance not obvious from the wire trace. "MD" (streaming scan) with a
count=1 works reliably and was verified byte-for-byte against a real scan:
1081 points (matches AMAX-AMIN+1), smoothly varying ~2.6-2.7m readings
across neighboring angles at the front, consistent with a real wall/object
-- so this driver uses MD for both single_scan() and iter_scans().

Response block layout for one MD scan (verified against a live capture):
  line0: "MD"+start+end+cluster+scan_count header (echoed/scan-index line)
  line1: 2-char status + 1-char checksum
  line2: 4-char encoded timestamp + 1-char checksum
  line3..N: 64 payload chars + 1-char checksum, until a blank line
Each 3 payload chars decode to one 18-bit distance-in-mm value (6 bits/char,
char value = ord(c) - 0x30). Values near 65535 (e.g. 65533/65535) are
Hokuyo's no-detection sentinels, not real distances.
"""
from __future__ import annotations

import socket
import time
from dataclasses import dataclass
from typing import Iterator

DEFAULT_HOST = "192.168.0.10"
DEFAULT_PORT = 10940

STEP_MIN = 0
STEP_MAX = 1080
STEP_FRONT = 540  # confirmed via PP's AFRT field
DEG_PER_STEP = 270.0 / 1080.0  # confirmed via PP's ARES/AMIN/AMAX fields

NO_DETECTION_SENTINELS = {65535, 65534, 65533, 0}


@dataclass
class ScanResult:
    distances_mm: list[int]  # index = raw step number (STEP_MIN..STEP_MAX)


def step_to_deg(step: int) -> float:
    return (step - STEP_FRONT) * DEG_PER_STEP


class HokuyoLidar:
    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT, timeout: float = 3.0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect((host, port))
        self._buf = b""
        self._streaming = False

    def _send(self, cmd: str) -> None:
        self.sock.sendall(cmd.encode("ascii") + b"\n")

    def _fill(self, timeout_s: float) -> None:
        self.sock.settimeout(timeout_s)
        chunk = self.sock.recv(65536)
        if chunk:
            self._buf += chunk

    def _read_block(self, timeout_s: float = 3.0) -> list[bytes]:
        """Reads until a blank line (double LF), returns the block's lines
        (as bytes, checksum bytes NOT stripped -- callers that need the
        payload strip the trailing checksum char themselves)."""
        deadline = time.monotonic() + timeout_s
        while b"\n\n" not in self._buf and time.monotonic() < deadline:
            self._fill(max(0.05, deadline - time.monotonic()))
        if b"\n\n" not in self._buf:
            lines = self._buf.split(b"\n")
            self._buf = b""
            return lines
        block, _, rest = self._buf.partition(b"\n\n")
        self._buf = rest
        return block.split(b"\n")

    def _parse_kv_block(self, lines: list[bytes]) -> dict:
        info = {}
        for line in lines[2:]:  # line0=echo, line1=status
            text = line.decode("ascii", errors="replace")
            if ":" in text:
                k, _, v = text.partition(":")
                info[k] = v.split(";")[0]
        return info

    def version(self) -> dict:
        self._send("VV")
        return self._parse_kv_block(self._read_block())

    def specs(self) -> dict:
        self._send("PP")
        return self._parse_kv_block(self._read_block())

    @staticmethod
    def _decode_payload(payload_lines: list[bytes]) -> list[int]:
        payload = "".join(line[:-1].decode("ascii", errors="replace")
                           for line in payload_lines if line)
        values = []
        for i in range(0, len(payload) - 2, 3):
            chunk = payload[i:i + 3]
            if len(chunk) < 3:
                break
            v = 0
            for c in chunk:
                v = (v << 6) | (ord(c) - 0x30)
            values.append(v)
        return values

    def single_scan(self, step_min: int = STEP_MIN, step_max: int = STEP_MAX) -> ScanResult | None:
        """One-shot scan via MD with scan_count=01 (GD returns a non-OK
        status on this unit, see module docstring)."""
        self._send(f"MD{step_min:04d}{step_max:04d}00001")  # cluster=00 interval=0 count=01
        ack = self._read_block()  # echo + "00"+checksum ack, discard
        if len(ack) < 2 or not ack[1].startswith(b"00"):
            return None
        data = self._read_block()
        if len(data) < 4:
            return None
        status = data[1][:2]
        if status not in (b"99", b"00"):  # both observed as "data follows" on this unit
            return None
        payload_lines = []
        for line in data[3:]:
            if line == b"":
                break
            payload_lines.append(line)
        distances = self._decode_payload(payload_lines)
        return ScanResult(distances_mm=distances)

    def iter_scans(self, step_min: int = STEP_MIN, step_max: int = STEP_MAX) -> Iterator[ScanResult]:
        """Continuous scanning (MD, scan_count=00 = infinite) until stop()
        is called (from another thread) or the generator is closed."""
        self._send(f"MD{step_min:04d}{step_max:04d}00000")  # cluster=00 interval=0 count=00(infinite)
        ack = self._read_block()
        if len(ack) < 2 or not ack[1].startswith(b"00"):
            return
        self._streaming = True
        try:
            while self._streaming:
                data = self._read_block()
                if len(data) < 4:
                    continue
                payload_lines = []
                for line in data[3:]:
                    if line == b"":
                        break
                    payload_lines.append(line)
                distances = self._decode_payload(payload_lines)
                if distances:
                    yield ScanResult(distances_mm=distances)
        finally:
            self._streaming = False

    def stop(self) -> None:
        self._streaming = False
        self._send("QT")
        time.sleep(0.05)
        self._buf = b""

    def close(self) -> None:
        try:
            self.stop()
        except Exception:
            pass
        self.sock.close()


def min_distance_in_frontal_arc(scan: ScanResult, half_width_deg: float = 15.0,
                                 min_valid_mm: int = 20) -> float | None:
    """Closest valid return within +/- half_width_deg of dead-ahead.
    Filters out Hokuyo's no-detection sentinel values (~65535) and returns
    below the sensor's rated minimum range (20mm)."""
    best = None
    half_steps = int(half_width_deg / DEG_PER_STEP)
    lo = max(STEP_MIN, STEP_FRONT - half_steps)
    hi = min(STEP_MAX, STEP_FRONT + half_steps)
    for step in range(lo, min(hi + 1, len(scan.distances_mm))):
        d = scan.distances_mm[step]
        if d < min_valid_mm or d in NO_DETECTION_SENTINELS:
            continue
        if best is None or d < best:
            best = d
    return best
