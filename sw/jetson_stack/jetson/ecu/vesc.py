"""VESC motor controller UART protocol -- BENCH-TEST / TELEMETRY TOOL ONLY.

ARCHITECTURE NOTE: in the production wiring, VESC is connected ONLY to the
STM32 gatekeeper (see gatekeeper/stm32/vesc_uart.c), never to the Jetson.
This is deliberate: the whole point of the safety architecture is that only
STM32-judged (gk_core_process()-approved) commands ever reach the motor, so
there must be no code path where the Jetson can drive VESC directly. This
module is kept around purely for bench debugging (reading telemetry off
VESC's USB port when it's temporarily also plugged into the Jetson for
testing) -- the set_duty/set_current/set_rpm/set_servo_pos methods below
must never be called from jetson/ecu/main_ecu.py or any other part of the
normal driving path.

Originally verified over /dev/ttyACM0 (VESC's USB-CDC port, ChibiOS/RT
Virtual COM Port, fw 6.6) while it was still bench-wired to this Jetson;
v_in read back as 15.1V, matching the real battery.

Implements the standard VESC serial packet framing (start byte, length,
payload, CRC16-CCITT, end byte) and a useful subset of commands. Telemetry
read (get_values/get_fw_version) is completely safe -- it never moves
anything. The drive commands (set_duty/set_current/set_rpm/set_brake_current)
DO physically spin the motor and are gated behind `arm=True` on the
VescLink constructor -- never flip that on without the vehicle safely
elevated/secured and someone ready to cut power.

COMM_GET_VALUES payload layout follows the commonly-deployed VESC firmware
(~4.x/5.x era) field order. If your specific firmware differs, the parsed
dict will be misaligned -- cross-check a couple of fields (v_in should read
your actual battery voltage) before trusting it for anything.
"""
from __future__ import annotations

import struct
import time
from dataclasses import dataclass

import serial

DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 115200

# COMM_PACKET_ID subset (see VESC firmware datatypes.h)
COMM_FW_VERSION = 0
COMM_GET_VALUES = 4
COMM_SET_DUTY = 5
COMM_SET_CURRENT = 6
COMM_SET_CURRENT_BRAKE = 7
COMM_SET_RPM = 8
COMM_ALIVE = 30


def _crc16(data: bytes) -> int:
    """CRC-16/XMODEM (poly 0x1021, init 0x0000) -- what VESC firmware uses."""
    crc = 0x0000
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def pack_frame(payload: bytes) -> bytes:
    if len(payload) <= 255:
        header = bytes([0x02, len(payload)])
    else:
        header = bytes([0x03, (len(payload) >> 8) & 0xFF, len(payload) & 0xFF])
    crc = _crc16(payload)
    return header + payload + bytes([(crc >> 8) & 0xFF, crc & 0xFF, 0x03])


@dataclass
class VescValues:
    temp_fet: float
    temp_motor: float
    current_motor: float
    current_in: float
    duty_now: float
    rpm: float
    v_in: float
    amp_hours: float
    amp_hours_charged: float
    watt_hours: float
    watt_hours_charged: float
    tachometer: int
    tachometer_abs: int
    fault_code: int


def _parse_get_values(payload: bytes) -> VescValues | None:
    # payload[0] == COMM_GET_VALUES, rest is big-endian fixed-point fields.
    # Confirmed against real hardware (fw 6.6, see get_fw_version()): modern
    # VESC firmware inserts FOC id/iq currents between current_in and
    # duty_now, and rpm is int32 (not int16) -- an earlier version of this
    # parser assumed the older ~4.x/5.x layout and silently misread v_in as
    # 0. Layout: temp_mos(h/10) temp_motor(h/10) current_motor(i/100)
    # current_in(i/100) id(i/100) iq(i/100) duty_now(h/1000) rpm(i)
    # v_in(h/10) amp_hours(i/1e4) amp_hours_charged(i/1e4) watt_hours(i/1e4)
    # watt_hours_charged(i/1e4) tachometer(i) tachometer_abs(i) fault(b)
    body = payload[1:]
    try:
        temp_fet_raw, temp_motor_raw, cur_motor_raw, cur_in_raw, _id_raw, _iq_raw = \
            struct.unpack_from(">hhiiii", body, 0)
        off = 2 + 2 + 4 + 4 + 4 + 4
        duty_raw, rpm_raw, vin_raw = struct.unpack_from(">hih", body, off)
        off += 2 + 4 + 2
        ah_raw, ahc_raw, wh_raw, whc_raw = struct.unpack_from(">iiii", body, off)
        off += 16
        tach_raw, tach_abs_raw = struct.unpack_from(">ii", body, off)
        off += 8
        fault_code = body[off] if off < len(body) else -1
    except struct.error:
        return None

    return VescValues(
        temp_fet=temp_fet_raw / 10.0,
        temp_motor=temp_motor_raw / 10.0,
        current_motor=cur_motor_raw / 100.0,
        current_in=cur_in_raw / 100.0,
        duty_now=duty_raw / 1000.0,
        rpm=float(rpm_raw),
        v_in=vin_raw / 10.0,
        amp_hours=ah_raw / 10000.0,
        amp_hours_charged=ahc_raw / 10000.0,
        watt_hours=wh_raw / 10000.0,
        watt_hours_charged=whc_raw / 10000.0,
        tachometer=tach_raw,
        tachometer_abs=tach_abs_raw,
        fault_code=fault_code,
    )


class VescLink:
    def __init__(self, port: str = DEFAULT_PORT, baud: int = DEFAULT_BAUD,
                 arm: bool = False, timeout: float = 0.5):
        self.ser = serial.Serial(port, baudrate=baud, timeout=timeout)
        self.armed = arm
        time.sleep(0.1)  # let ChibiOS USB-CDC settle after open

    def _send(self, payload: bytes) -> None:
        self.ser.write(pack_frame(payload))

    def _read_frame(self, timeout_s: float = 0.5) -> bytes | None:
        deadline = time.monotonic() + timeout_s
        # Minimal reader: assumes short frames (start byte 0x02). Robust
        # enough for telemetry polling; not a hardened parser for noisy
        # links.
        start = self.ser.read(1)
        if not start or start[0] != 0x02:
            return None
        length_b = self.ser.read(1)
        if not length_b:
            return None
        length = length_b[0]
        rest = self.ser.read(length + 3)  # payload + crc(2) + end(1)
        if len(rest) != length + 3:
            return None
        payload = rest[:length]
        crc_recv = (rest[length] << 8) | rest[length + 1]
        end = rest[length + 2]
        if end != 0x03 or _crc16(payload) != crc_recv:
            return None
        return payload

    def get_fw_version(self) -> tuple[int, int] | None:
        self._send(bytes([COMM_FW_VERSION]))
        p = self._read_frame()
        if p is None or len(p) < 3:
            return None
        return p[1], p[2]

    def get_values(self) -> VescValues | None:
        """Safe: read-only telemetry, never moves the motor."""
        self._send(bytes([COMM_GET_VALUES]))
        p = self._read_frame()
        if p is None:
            return None
        return _parse_get_values(p)

    def alive(self) -> None:
        """VESC firmware (if configured with a timeout) expects periodic
        COMM_ALIVE or COMM_SET_* traffic, else it cuts the motor for
        safety. Call this at a few Hz if you're not otherwise sending
        drive commands but want to keep a connection considered 'alive'."""
        self._send(bytes([COMM_ALIVE]))

    def _require_armed(self):
        if not self.armed:
            raise RuntimeError(
                "VescLink is not armed -- refusing to send a drive command. "
                "Construct with arm=True only when the vehicle is safely "
                "secured and you intend the wheels to actually move.")

    def set_duty(self, duty: float) -> None:
        """duty in [-1, 1]. PHYSICALLY SPINS THE MOTOR."""
        self._require_armed()
        duty = max(-1.0, min(1.0, duty))
        payload = bytes([COMM_SET_DUTY]) + struct.pack(">i", int(duty * 100000))
        self._send(payload)

    def set_current(self, amps: float) -> None:
        """PHYSICALLY SPINS THE MOTOR."""
        self._require_armed()
        payload = bytes([COMM_SET_CURRENT]) + struct.pack(">i", int(amps * 1000))
        self._send(payload)

    def set_current_brake(self, amps: float) -> None:
        """Regenerative braking current. PHYSICALLY AFFECTS THE MOTOR."""
        self._require_armed()
        payload = bytes([COMM_SET_CURRENT_BRAKE]) + struct.pack(">i", int(amps * 1000))
        self._send(payload)

    def set_rpm(self, erpm: int) -> None:
        """PHYSICALLY SPINS THE MOTOR."""
        self._require_armed()
        payload = bytes([COMM_SET_RPM]) + struct.pack(">i", int(erpm))
        self._send(payload)

    def close(self) -> None:
        self.ser.close()
