# Jetson side

Everything that runs on the Jetson Orin Nano, the AI computer of the car. The Jetson
plans the path and sends drive commands; it has no wire to the motor. The STM32N6570-DK
running μT-Kernel 3.0 decides whether those commands reach the VESC — see the
[top-level README](../../README.md).

```
ros2/         what runs on the car today (ROS 2 Humble)
bench/        jitter_probe.c: the Linux side of the RTOS-vs-Linux comparison
gatekeeper/   the safety logic in plain C, and the SIL rig it was developed in
jetson/       the earlier software-in-the-loop ECU and threat scripts
docs/         link protocol, and the porting notes we worked from
```

## ros2/ — driving the car

Installed on the Jetson at `~/guardian/`. The evaluation runbook in the top-level README
uses only `hibiki.sh`.

| File | What |
|---|---|
| `hibiki.sh` | one-word commands: `start [speed] [cruise\|gap]`, `arm`, `disarm`, `stress`, `freeze`, `status`, `stop` |
| `gt_bridge.py` | ROS 2 node: `/drive` → 15-byte CMD at 100 Hz over `/dev/ttyUSB0`, and back from the 12-byte VERDICT. Counts approved / vetoed / malformed and the round-trip time |
| `gt_cruise.py` | constant-speed straight-line controller, for repeatable timing runs |
| `guardian_gap.launch.py` | brings up the LiDAR (`urg_node`), the planner (gap follower or cruise) and the bridge |

The STM32 owns the motor, so `vesc_driver` must never run next to this stack.

`/dev/ttyUSB0` is the USB-TTL adapter. Its PL2303 driver is not in the stock L4T kernel,
so we built the module and load it at boot; if the device node is missing after a reboot,
see step 3 of [`sw/docs/setup_guide.md`](../docs/setup_guide.md). Unplugging and replugging
the adapter also brings it back.

## bench/ — the Linux comparison

`jitter_probe.c` runs the same 100 Hz periodic loop as the STM32's safety monitor, with
either a relative or an absolute sleep, and reports the period error.

```bash
gcc -O2 -o jitter_probe bench/jitter_probe.c
./jitter_probe abs 20                     # then again under: stress --cpu $(nproc) --io 2 --vm 2
```

Our measurements are in [`sw/results/linux_jitter_jetson.txt`](../results/linux_jitter_jetson.txt)
and section 3 of the [test report](../docs/test_report.md).

## gatekeeper/ — the safety logic, and where it grew up

Hardware-independent C11 with no HAL and no RTOS calls, so the same source builds for
Linux and for the board. `gatekeeper_core.c` and `protocol.c` are **byte-identical** to
[`sw/guardian_vision/gt/`](../guardian_vision/gt/), the copy the firmware compiles.
`safety_envelope.c` is the one file that differs: the firmware's version carries the
limits we measured on this car (steering capped at ±17.5°, speed −0.5…2.0 m/s), while the
copy here keeps the generic limits the SIL rig was written against.

`sim/` runs that logic on a Linux pseudo-terminal, so the protocol and the veto decisions
can be exercised without a board (`make sim`). `stm32/` holds the bare-metal bridges from
before the μT-Kernel port; the firmware now uses its own `gt_uart.c` / `gt_vesc.c`
instead, and these are kept for reference.

## jetson/ — earlier software-in-the-loop work

The ECU loop and three threat scenarios (adversarial perception, link spoofing and
replay, OTA signature tampering) that we used to develop the gatekeeper before the board
was wired. Not part of the contest measurements, kept because the replay and malformed-frame
defences in the shipped firmware came out of running them. `jetson/ecu/vesc.py` and
`jetson/ecu/lidar.py` were verified against the real hardware.

These same three threats were also driven against a simulated car in CARLA, which is where
we confirmed the veto logic works and where we found the limitation that made us give the
STM32 its own sensors: [`docs/CARLA_SIL.md`](docs/CARLA_SIL.md).

For faults injected into the *shipped* system instead, use `pc_host.py --scenario` (see
[`sw/tools/`](../tools/)) or `hibiki.sh stress | freeze`.

## docs/

| File | Status |
|---|---|
| [`PROTOCOL.md`](docs/PROTOCOL.md) | current — the 15-byte CMD / 12-byte VERDICT frames, CRC and sequence rules, as shipped |
| [`STM32_PORT.md`](docs/STM32_PORT.md) | historical — the porting plan we worked from. The port is finished; [`hw/wiring.md`](../../hw/wiring.md) and [`sw/docs/setup_guide.md`](../docs/setup_guide.md) describe what was actually built |
| [`WIRING.md`](docs/WIRING.md) | historical — the open questions before the car was wired. Superseded by [`hw/wiring.md`](../../hw/wiring.md) |
