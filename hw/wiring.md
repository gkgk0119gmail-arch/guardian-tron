# Wiring

All connections below were checked on the board. Signal levels are 3.3 V.

```
                         USB (ttyUSB0)                     Arduino header
 Jetson Orin Nano ──── USB-TTL (PL2303, 3.3 V) ─── TXD ──► D0  PF6  USART2_RX ┐
 (ROS 2 planner,                                   RXD ◄── D1  PD5  USART2_TX │  STM32N6570-DK
  Hokuyo LiDAR on                                  GND ─── GND                │  μT-Kernel 3.0
  Ethernet)                                                                    │
                                                   red ◄── D5  PE10 USART3_TX │  (TX/RX swapped
 VESC 6 MkVI COMM port ◄──────────────────────── brown ──► A5  PB10 USART3_RX │   in software)
   ├─ motor (BLDC)                                 black ── GND               ┘
   └─ steering servo (servo header)
                                                    Camera module MB1854 on the camera
                                                    connector (FFC): IMX335 (I2C1 0x1A),
                                                    ToF (0x29), IMU (0x6A)
```

## Links

| Link | STM32 pins | Peripheral | Settings | Peer |
|---|---|---|---|---|
| Jetson commands | D0 = PF6 (RX), D1 = PD5 (TX), GND | USART2 | 115200 8N1, RX interrupt + FIFO, TX interrupt | USB-TTL: TXD → D0, RXD → D1, GND → GND. **Leave the adapter's 5 V pin unconnected** |
| VESC | D5 = PE10 (TX), A5 = PB10 (RX), GND | USART3 with `USART_CR2_SWAP` | **38400** 8N1 | VESC COMM: RX ← D5 (red), TX → A5 (brown), GND (black) |
| Console / logs | ST-LINK virtual COM port (USB-C) | USART1 | 115200 8N1 | PC terminal (`/dev/cu.usbmodem*`, `COMx`, `/dev/ttyACM*`) |
| Camera, ToF, IMU | camera FFC connector | I2C1 at 400 kHz, CSI-2 + DCMIPP | — | MB1854 camera module |
| Logic-analyzer probes (optional) | D2 = PD0, D3 = PE9, D4 = PH5 | GPIO outputs | — | D2 HIGH while a hazard holds the car; D3 toggles every 10 ms monitor period; D4 HIGH while one Jetson command is processed |

Why the VESC link is swapped and slow: on the DK board, A5 (PB10) sits behind the analog-input network (series resistor + pull-down). It works as an input but cannot drive a line. So USART3 TX/RX are swapped and D5 transmits. The RC filter on A5 limits reception to 38400 baud. The VESC app UART is set to 38400 (persisted in the VESC configuration).

## VESC configuration (VESC Tool, firmware 6.6, hardware 60_MK6_HP)

- App to use: UART, permanent UART on, UART baud **38400**
- Servo output enabled (steering servo on the VESC servo header)
- Calibration used by the firmware (`gt/gt_gate.c`): servo center 0.423, servo gain −0.89 /rad (lower = left), limits 0.15–0.85; speed → ERPM −4403 per m/s (forward = negative ERPM)

## Power

| Consumer | Source |
|---|---|
| VESC (motor, servo) | 4S LiPo (≈ 15 V) |
| Jetson Orin Nano, Hokuyo LiDAR | 4S LiPo via WAGO 221 splitters |
| STM32N6570-DK | 5 V USB into the **ST-LINK USB-C** port: a PC on a desk, or a USB power bank / 5 V converter on the car. Never from the Jetson's USB |

Ground: the STM32 GND is tied to the VESC GND (black wire) and to the USB-TTL GND. Do not power the board from a 5 V converter and a PC USB cable at the same time.

## Board switches

| Mode | BOOT0 | BOOT1 |
|---|---|---|
| Boot from flash (normal use, as shipped) | left | left |
| Development (debugger loads RAM, programming the flash) | left | right |
