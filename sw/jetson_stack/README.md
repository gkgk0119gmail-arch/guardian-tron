# SDV Safety Coprocessor — Runnable Stack

Runnable implementation of the Guardian-TRON heterogeneous safety coprocessor
project for SDVs. The Jetson (main AI ECU) and the STM32N6570-DK (safety
gatekeeper) are linked over UART, so normal driving and three threat scenarios
can actually be run end to end.

## What works right now (SIM mode)

Even without the real STM32 board, the gatekeeper's **actual safety-logic C
code** is compiled natively on this Jetson and run behind a virtual UART (PTY),
and the Jetson ECU/threat scripts exchange real UART frames with it.
`gatekeeper/src/*.c` is the same code that goes onto the real board unchanged
(see `docs/STM32_PORT.md` for details), so the safety decision logic verified
here behaves identically on the real board.

```bash
cd /home/orin/미래모빌리티

# 1) Normal driving, 15 seconds
./jetson/scripts/run_vehicle.sh 15

# 2) Threat 1: FGSM adversarial perception attack
./jetson/scripts/run_threat1.sh

# 3) Threat 2: UART link spoofing/replay/fuzzing
./jetson/scripts/run_threat2.sh

# 4) Threat 3: OTA/firmware signature tampering vs root of trust
./jetson/scripts/run_threat3.sh
```

## Moving to the real board (STM32N6570-DK)

Physical wiring is already verified and complete: the Jetson (`/dev/ttyUSB0`)
is wired directly to STM32N6570-DK Arduino D0/D1 (USART2) through a USB-TTL
adapter, 115200 8N1 (see the "Verified" table in `docs/STM32_PORT.md`).

1. `docs/STM32_PORT.md` — create the real-board project with STM32CubeIDE/CubeMX
   (enable USART2), port `gatekeeper/src/*.c` + `gatekeeper/stm32/*.c`
   unchanged, and integrate μT-Kernel 3.0 BSP2.
2. After porting, just add `--real` to each script and it connects to the real
   board (`/dev/ttyUSB0`) instead of the SIM gatekeeper:
   ```bash
   ./jetson/scripts/run_vehicle.sh 15 --real
   ```
3. After a reboot, the PL2303 kernel module must be reloaded for `/dev/ttyUSB0`
   to appear (it is not included in this Jetson's kernel by default, so we built
   it ourselves, `toolchain/pl2303-build/`):
   ```bash
   sudo modprobe usbserial
   sudo insmod /home/orin/미래모빌리티/toolchain/pl2303-build/pl2303.ko
   sudo udevadm trigger
   ```

## Layout

```
gatekeeper/            Gatekeeper core logic (hardware-independent C, ported to the real board unchanged)
  include/              protocol.h, safety_envelope.h, gatekeeper_core.h
  src/                   Implementation
  sim/main_sim.c         SIM runner on a Linux PTY (not ported)
  sim/main_udp.c         UDP SIM runner (for the Ethernet path, not ported)
  stm32/                 Real-board bridges (added to the CubeIDE project as-is)
    gatekeeper_uart_bridge.c/h   USART2 interrupt-driven bridge (the adopted path)
    gatekeeper_lwip_udp.c/h      lwIP-based bridge (alternative Ethernet path)
    dwt_timing.c/h                Shared DWT cycle-counter utility
    vesc_uart.c/h                 C port of the VESC protocol (STM32-only drive path)
    local_safety_monitor.c/h      STM32 onboard camera + NPU collision-probability stub
  Makefile               make sim / make udp

jetson/
  ecu/                  Main AI ECU (perception inference + UART/UDP transmit)
    perception.py        Lightweight neural net (numpy, real gradient-based FGSM possible)
    protocol.py           Byte-for-byte identical to gatekeeper/include/protocol.h
    uart_link.py           pyserial wrapper (default transport, /dev/ttyUSB0)
    udp_link.py             UDP socket wrapper (alternative path)
    link_factory.py          Shared helper to select either transport from the CLI
    vesc.py                  VESC motor controller UART protocol (verified on hardware)
    lidar.py                 Hokuyo UST-10LX Ethernet driver (verified on hardware)
    main_ecu.py               Normal driving loop
  threats/
    threat1_fgsm.py        Threat 1: adversarial perception attack
    threat2_injection.py   Threat 2: link spoofing/replay/fuzzing
    threat3_ota_tamper.py  Threat 3: OTA signature tampering vs RoT
  scripts/               run_*.sh launchers

toolchain/
  pl2303-build/          Build of the PL2303 USB-TTL driver missing from this Jetson's kernel

docs/
  PROTOCOL.md            Frame format, safety envelope definition
  WIRING.md              Wiring checklist (early investigation; STM32_PORT.md is current)
  STM32_PORT.md          Real-board porting roadmap, confirmed wiring, current limitations
```

## Safety decision logic (paper Sec. IV.3, implementation of Eq. 2-4)

- Absolute limits: `|steer_deg| <= 540`, `accel_mps2 ∈ [-10, 4]`
- Rate-of-change limits (at 100Hz): `|Δsteer_deg| <= 10`, `|Δaccel_mps2| <= 1`
- On violation, hold the last approved command (u_safe) and transition to fail-safe
- Additionally: on sequence replay/rollback detection, veto regardless of whether
  the envelope check passes (`gatekeeper/src/gatekeeper_core.c`) — a defense added
  after actually running Threat 2 revealed that the envelope alone cannot stop
  "retransmissions that look like normal values".

## Known limitations (honestly)

- The FGSM attack's abrupt step changes are reliably blocked (rate-of-change
  limit), but, as paper Sec. VI.2 itself acknowledges, **slowly accumulating
  small drift cannot be caught by the gatekeeper alone** — running
  `run_threat1.sh` shows the onset of the attack being VETOed, then switching to
  APPROVED once it holds at a similar magnitude. This is not a bug; under the
  defense-in-depth philosophy it must be covered by robustness at the perception
  stage.
- perception.py is not the paper's actual CUDA PilotNet but a two-layer neural
  net written in numpy (torch is not installed on this Jetson; installing the
  Jetson-specific wheel is separate work). It can be swapped for the real
  PilotNet as long as the `predict()`/`input_gradient()` interface matches.
- μT-Kernel 3.0, the actual STM32N6 secure-boot header, and the NPU IDS are not
  yet ported — see the `docs/STM32_PORT.md` roadmap.
