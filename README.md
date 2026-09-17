# Guardian-TRON — heterogeneous safety co-processor for an autonomous RC car (μT-Kernel 3.0 / STM32N6570-DK)

Entry for **TRON Programming Contest 2026 — RTOS Application (Student)**.
Theme: *TRON × AI*.

A Jetson Orin Nano plans the path from a Hokuyo LiDAR (ROS 2 / F1TENTH stack). It never touches the
actuators. Every command goes through an **STM32N6570-DK running μT-Kernel 3.0**, which

* validates each command against a safety envelope (limits, rate limits, CRC, replay protection) and
  only then drives the VESC motor controller (steering servo + motor) — ~1.2 ms verdict latency,
* stops the car by itself if the Jetson goes silent for 200 ms (watchdog task),
* watches the road with the board's own camera module (IMX335 + Neural-ART NPU person detection,
  8×8 ToF, IMU) and overrides the Jetson when a collision is imminent.

```
Jetson (LiDAR, ROS 2 planner) ──UART 115200──▶ STM32N6570-DK (μT-Kernel 3.0)
                                                  ├─ gate task: envelope → VESC (UART 38400)
                                                  ├─ watchdog: 200 ms → brake
                                                  └─ local safety: camera/NPU + ToF → override
                                                                  │
                                                             VESC 6 MkVI → motor + steering servo
```

## Repository layout

| Path | What |
|---|---|
| `mtk3bsp2_stm32n657/` | STM32CubeIDE project (FSBL + Appli). Application code is in `Appli/Application/` |
| `mtk3bsp2_stm32n657/Appli/Application/gt_gate.c` | Gatekeeper task: CMD framing, verdict, VESC actuation, watchdog |
| `.../gatekeeper_core.c`, `safety_envelope.c`, `protocol.c` | Hardware-independent safety logic (also runs natively on the Jetson for simulation) |
| `.../gt_uart.c`, `gt_vesc.c` | Register-level UART driver and VESC protocol |
| `jetson_stack/` | Jetson side: ECU client, protocol mirror, threat-scenario scripts, ROS 2 bridge |
| `prj_stm32n6_cam/` | TRON Forum camera example (IMX335 → LCD) used as the camera pipeline reference |
| `tools/` | `load_ram.sh` (RAM load via STM32CubeProgrammer), `serial.sh`, ST-LINK reset helper |
| `docs/` | Operation manual, operation procedure, wiring, list of third-party software, photos |

## Documents (for the judges)

* [Operation procedure — how to power up and run](docs/operation_procedure.md)
* [Operation manual — what the system does and how to use it](docs/operation_manual.md)
* [Wiring](docs/wiring.md)
* [Third-party software used](docs/third_party_software.md)

## Team

Yun-sang Nam (system architecture, integration), Min-jun Song (control / RTOS), Na-yeon Kwak (AI).

## License

Our own code (`Appli/Application/*`, `jetson_stack/*`, `tools/*`, `docs/*`) is released under the MIT License.
μT-Kernel 3.0 BSP2 is under the T-License 2.2 (TRON Forum); STM32 HAL/BSP/middleware under
STMicroelectronics' licenses — see `docs/third_party_software.md`.
