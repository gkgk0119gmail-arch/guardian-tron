# Real-Board Wiring — early investigation

> **Status: historical.** These were the open questions before the car was wired. They
> are all answered now — the pins, colours, baud rates and board switches as built are in
> [`hw/wiring.md`](../../../hw/wiring.md), with a cable colour map in
> [`hw/photos/pinmap.svg`](../../../hw/photos/pinmap.svg).

SIM mode currently runs the gatekeeper inside the Jetson over a virtual UART
(PTY), so no physical wiring is needed. **For in-vehicle integration, the items
below must be measured and confirmed** — wiring to the wrong pins could, in the
worst case, leave the gatekeeper unresponsive or damage the board, so we do not
proceed on guesswork.

## To confirm

1. **Which UART**: the STM32N6570-DK exposes UART on both the Arduino connector
   (some CN) and the ST-Link virtual COM port (USB). The ST-Link VCP is a
   USB-UART bridge, so it needs no wiring and shows up as e.g. `/dev/ttyACM0`
   (this time `lsusb` already showed `0483:5740 STMicroelectronics Virtual COM Port`
   — this is most likely the ST-Link VCP). To use an actual GPIO UART on the
   Arduino header (e.g. USART1 TX/RX), the exact pin numbers must be checked in
   the connector pinout of the STM32N6570-DK user manual (UM3392).
2. **Jetson-side UART**: decide whether to use the Jetson Orin Nano 40-pin
   header UART (pins 8/10, `/dev/ttyTHS*` etc.) or USB-UART (ST-Link VCP,
   `/dev/ttyACM0`). The latter needs no wiring, is much simpler and leaves less
   room for error — unless there is a specific reason otherwise,
   **going through the ST-Link USB VCP is recommended**.
3. **Baud rate**: set to a default of 115200 8N1 (`jetson/ecu/uart_link.py`
   `DEFAULT_BAUD`, `docs/PROTOCOL.md`). The HAL UART settings on the STM32 side
   must match.
4. **Common ground**: when wiring GPIO UART directly, the GND of the Jetson and
   the STM32 board must be connected in common (via USB VCP, the USB cable
   already handles this).

## Once confirmed

Pass the actual device path with the `--real-port` option, as in
`jetson/scripts/run_vehicle.sh 15 --real-port /dev/ttyACM0`, and it connects
directly to that port without launching the SIM gatekeeper (the gatekeeper
firmware from `docs/STM32_PORT.md` must already be flashed on the STM32 side).
