"""Thin serial wrapper shared by the ECU main loop and the threat scripts.

Real, wired link confirmed: STM32N6570-DK Arduino connector D0/D1 (USART2,
PF6/PD5) <-> USB-to-TTL adapter (PL2303 chip; needed a custom-built
pl2303.ko, see toolchain/pl2303-build/) <-> this Jetson's /dev/ttyUSB0.
115200 8N1. Point at the SIM gatekeeper's published symlink instead
(default /tmp/stm32_gatekeeper) when running gatekeeper/build/gatekeeper_sim
locally without the board.
"""
from __future__ import annotations

import time

import serial

from . import protocol as proto

DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 115200


class GatekeeperLink:
    def __init__(self, port: str = DEFAULT_PORT, baud: int = DEFAULT_BAUD, timeout: float = 0.5):
        self.ser = serial.Serial(port, baudrate=baud, timeout=timeout)

    def send_cmd(self, cmd: proto.Cmd) -> None:
        self.ser.write(cmd.encode())

    def send_raw(self, raw: bytes) -> None:
        """Used by threat scripts that need to inject malformed/spoofed
        bytes directly, bypassing Cmd.encode()'s well-formedness."""
        self.ser.write(raw)

    def recv_verdict(self, timeout_s: float = 0.5) -> proto.VerdictFrame | None:
        deadline = time.monotonic() + timeout_s
        buf = bytearray()
        while time.monotonic() < deadline:
            chunk = self.ser.read(1)
            if not chunk:
                continue
            if not buf and chunk[0] != proto.VERDICT_STX:
                continue
            buf += chunk
            if len(buf) == proto.VERDICT_FRAME_LEN:
                vf = proto.VerdictFrame.decode(bytes(buf))
                if vf is not None:
                    return vf
                buf = buf[1:]  # resync
        return None

    def close(self) -> None:
        self.ser.close()
