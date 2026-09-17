"""UDP transport for the safety-critical Jetson<->STM32 link, matching the
actual physical wiring (Ethernet/RJ45) and the paper's original raw-UDP
safety link design (port 5005). Same interface shape as uart_link.py's
GatekeeperLink so main_ecu.py / threat scripts can use either.
"""
from __future__ import annotations

import socket

from . import protocol as proto

DEFAULT_HOST = "127.0.0.1"   # gatekeeper/build/gatekeeper_udp running locally
                               # until the STM32 is flashed and on the network
DEFAULT_PORT = 5005


class GatekeeperUdpLink:
    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send_cmd(self, cmd: proto.Cmd) -> None:
        self.sock.sendto(cmd.encode(), self.addr)

    def send_raw(self, raw: bytes) -> None:
        """Used by threat scripts that need to inject malformed/spoofed
        bytes directly, bypassing Cmd.encode()'s well-formedness."""
        self.sock.sendto(raw, self.addr)

    def recv_verdict(self, timeout_s: float = 0.5) -> proto.VerdictFrame | None:
        self.sock.settimeout(timeout_s)
        try:
            data, _ = self.sock.recvfrom(256)
        except socket.timeout:
            return None
        return proto.VerdictFrame.decode(data)

    def close(self) -> None:
        self.sock.close()
