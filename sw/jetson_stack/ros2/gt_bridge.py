#!/usr/bin/env python3
"""gt_bridge - F1TENTH /ackermann_cmd -> Guardian-TRON STM32 gatekeeper.

Replaces vesc_driver + ackermann_to_vesc: the STM32 owns the VESC, so every
drive command must pass the gatekeeper. Sends a 15-byte CMD at a fixed rate
(steer_deg = steering_angle[rad] * 57.3, 2nd float = target speed [m/s]) and
reads 12-byte VERDICT frames back. Protocol mirrors jetson/ecu/protocol.py.

Do NOT run vesc_driver / ackermann_to_vesc_node at the same time.
"""
import struct
import time

import rclpy
import serial
from ackermann_msgs.msg import AckermannDriveStamped
from rclpy.node import Node
from std_msgs.msg import String

RAD2DEG = 57.29577951308232
CMD_STX, VERDICT_STX, ETX = 0xAA, 0xBB, 0x55
VERDICT_NAMES = {0: 'APPROVED', 1: 'VETO', 2: 'MALFORMED'}


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def open_port(port: str, baud: int) -> serial.Serial:
    # PL2303 quirk on this Jetson: the first open after boot does not take the
    # line settings, so open once, close, and reopen.
    serial.Serial(port, baud, timeout=0).close()
    time.sleep(0.2)
    s = serial.Serial(port, baud, timeout=0)
    time.sleep(0.2)
    s.reset_input_buffer()
    return s


class GtBridge(Node):
    def __init__(self):
        super().__init__('gt_bridge')
        self.port = self.declare_parameter('port', '/dev/ttyUSB0').value
        self.baud = self.declare_parameter('baud', 115200).value
        topic = self.declare_parameter('topic', 'ackermann_cmd').value   # ackermann_mux output
        rate = self.declare_parameter('rate_hz', 100.0).value
        self.cmd_timeout = self.declare_parameter('cmd_timeout', 0.2).value

        self.ser = open_port(self.port, self.baud)
        # The gatekeeper rejects seq <= last seen seq (replay protection), and it
        # keeps that state across bridge restarts. Seed from monotonic ms so a
        # restarted bridge always continues above the previous run.
        self.seq = int(time.monotonic() * 1000) & 0xFFFFFFFF
        self.steer_rad = 0.0
        self.speed = 0.0
        self.last_msg = 0.0
        self.rx = b''
        self.stats = {'sent': 0, 'APPROVED': 0, 'VETO': 0, 'MALFORMED': 0}
        self.lat_sum = 0
        self.lat_n = 0
        self.lat_max = 0
        self.last_verdict = 0.0

        self.create_subscription(AckermannDriveStamped, topic, self.on_drive, 10)
        self.pub = self.create_publisher(String, 'guardian/verdict', 10)
        self.create_timer(1.0 / rate, self.tick)
        self.create_timer(1.0, self.report)
        self.get_logger().info(f'{topic} -> {self.port}@{self.baud}, {rate:.0f} Hz, seq start {self.seq}')

    def on_drive(self, msg: AckermannDriveStamped):
        self.steer_rad = msg.drive.steering_angle
        self.speed = msg.drive.speed
        self.last_msg = time.monotonic()

    def tick(self):
        fresh = time.monotonic() - self.last_msg < self.cmd_timeout
        steer_deg = self.steer_rad * RAD2DEG if fresh else 0.0
        speed = self.speed if fresh else 0.0          # stale input -> command a stop
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        body = struct.pack('<Iff', self.seq, steer_deg, speed)
        try:
            self.ser.write(bytes([CMD_STX]) + body + bytes([crc8(body), ETX]))
            self.stats['sent'] += 1
            self.rx += self.ser.read(self.ser.in_waiting or 0)
        except serial.SerialException as e:
            self.get_logger().error(f'serial error: {e}; reopening')
            try:
                self.ser.close()
                self.ser = open_port(self.port, self.baud)
            except serial.SerialException:
                pass
            return
        self.parse()

    def parse(self):
        while True:
            i = self.rx.find(bytes([VERDICT_STX]))
            if i < 0:
                self.rx = b''
                return
            if len(self.rx) - i < 12:
                self.rx = self.rx[i:]
                return
            f = self.rx[i:i + 12]
            if f[11] != ETX or crc8(f[1:10]) != f[10]:
                self.rx = self.rx[i + 1:]
                continue
            self.rx = self.rx[i + 12:]
            _, verdict, lat = struct.unpack('<IBI', f[1:10])
            name = VERDICT_NAMES.get(verdict, 'MALFORMED')
            self.stats[name] += 1
            self.last_verdict = time.monotonic()
            if lat < 1_000_000:                       # ignore the post-boot outlier
                self.lat_sum += lat
                self.lat_n += 1
                self.lat_max = max(self.lat_max, lat)
            if name != 'APPROVED':
                self.pub.publish(String(data=name))

    def report(self):
        s = self.stats
        avg = self.lat_sum / self.lat_n if self.lat_n else 0
        link = 'OK' if time.monotonic() - self.last_verdict < 0.5 else 'NO VERDICTS'
        fresh = time.monotonic() - self.last_msg < self.cmd_timeout
        self.get_logger().info(
            f"link={link} input={'live' if fresh else 'none->stop'} steer={self.steer_rad:+.3f}rad "
            f"v={self.speed:+.2f} | sent={s['sent']} ok={s['APPROVED']} veto={s['VETO']} "
            f"bad={s['MALFORMED']} lat avg={avg:.0f}us max={self.lat_max}us")


def main():
    rclpy.init()
    node = GtBridge()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        # leave the car stopped: a few zero-speed frames, then the STM32
        # watchdog (200 ms) holds it stopped anyway
        for _ in range(5):
            node.seq = (node.seq + 1) & 0xFFFFFFFF
            body = struct.pack('<Iff', node.seq, 0.0, 0.0)
            try:
                node.ser.write(bytes([CMD_STX]) + body + bytes([crc8(body), ETX]))
            except serial.SerialException:
                break
            time.sleep(0.01)
        node.ser.close()
        node.destroy_node()
        rclpy.try_shutdown()


if __name__ == '__main__':
    main()
