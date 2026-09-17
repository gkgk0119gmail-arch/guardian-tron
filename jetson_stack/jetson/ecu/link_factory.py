"""Shared CLI plumbing so main_ecu.py and every threat script build their
link the same way. Defaults to UART (/dev/ttyUSB0), the actual wired link
confirmed working between this Jetson and the STM32N6570-DK (USB-to-TTL
adapter -> Arduino D0/D1 -> USART2). UDP/Ethernet remains available via
--transport udp for the separate Hokuyo LiDAR-style network path or a
future Ethernet-wired gatekeeper."""
from __future__ import annotations

import argparse

from .uart_link import DEFAULT_PORT as UART_DEFAULT_PORT, DEFAULT_BAUD


def add_link_args(ap: argparse.ArgumentParser) -> None:
    ap.add_argument("--transport", choices=["uart", "udp"], default="uart")
    ap.add_argument("--uart-port", default=UART_DEFAULT_PORT, help="[uart] serial device")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="[uart] baud rate")
    ap.add_argument("--host", default="127.0.0.1", help="[udp] gatekeeper IP")
    ap.add_argument("--udp-port", type=int, default=5005)


def make_link(args):
    if args.transport == "uart":
        from .uart_link import GatekeeperLink
        return GatekeeperLink(port=args.uart_port, baud=args.baud)
    from .udp_link import GatekeeperUdpLink
    return GatekeeperUdpLink(host=args.host, port=args.udp_port)
