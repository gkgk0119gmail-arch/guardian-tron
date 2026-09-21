"""Byte-for-byte mirror of gatekeeper/include/protocol.h.

Keep this in sync with the C header manually -- there is no code generator
here, so any field change on one side must be mirrored on the other.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum

CMD_STX = 0xAA
VERDICT_STX = 0xBB
ETX = 0x55

CMD_FRAME_LEN = 15
VERDICT_FRAME_LEN = 12

_CMD_STRUCT = struct.Struct("<BIffBB")       # stx, seq, steer, accel, crc, etx
_VERDICT_STRUCT = struct.Struct("<BIBIBB")   # stx2, seq, verdict, latency_us, crc, etx2


class Verdict(IntEnum):
    APPROVED = 0
    VETO = 1
    MALFORMED = 2


def crc8(data: bytes) -> int:
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


@dataclass
class Cmd:
    seq: int
    steer_deg: float
    accel_mps2: float

    def encode(self) -> bytes:
        body = struct.pack("<Iff", self.seq, self.steer_deg, self.accel_mps2)
        crc = crc8(body)
        return bytes([CMD_STX]) + body + bytes([crc, ETX])

    @staticmethod
    def decode(buf: bytes) -> "Cmd | None":
        if len(buf) != CMD_FRAME_LEN:
            return None
        if buf[0] != CMD_STX or buf[14] != ETX:
            return None
        body = buf[1:13]
        if crc8(body) != buf[13]:
            return None
        seq, steer, accel = struct.unpack("<Iff", body)
        return Cmd(seq=seq, steer_deg=steer, accel_mps2=accel)


@dataclass
class VerdictFrame:
    seq: int
    verdict: Verdict
    latency_us: int

    @staticmethod
    def decode(buf: bytes) -> "VerdictFrame | None":
        if len(buf) != VERDICT_FRAME_LEN:
            return None
        if buf[0] != VERDICT_STX or buf[11] != ETX:
            return None
        body = buf[1:10]
        if crc8(body) != buf[10]:
            return None
        seq, verdict, latency_us = struct.unpack("<IBI", body)
        return VerdictFrame(seq=seq, verdict=Verdict(verdict), latency_us=latency_us)
