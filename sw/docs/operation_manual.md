# Operation Manual

## What the system does

The Jetson (AI computer) plans the path but has no electrical path to the motor or the steering. The STM32N6570-DK running μT-Kernel 3.0 sits between them and decides, on its own deadlines, whether each command may reach the VESC motor controller:

| Situation | STM32 reaction | Log line |
|---|---|---|
| Normal driving | command checked (CRC, sequence, steering ±17.5°, speed −0.5…2.0 m/s, rate limits) and forwarded | `[gate] n=… ok=… DRIVING` |
| Command outside the envelope | clamped to the envelope, `VETO` returned to the Jetson | `veto=` counter |
| Corrupted / replayed command | dropped, `MALFORMED` / replay counter | `bad=`, `replay=` |
| Person 2–3.3 m ahead (camera + NPU) | speed capped so the car can still stop before 2 m: v = √(2 · 0.1 m/s² · (d − 2 m)) | `[gate] SLOW: person ~247 cm ahead … speed cap 309 mm/s` |
| Person within ≈ 2 m | **brake**, steering centred, held 1.5 s after the person leaves | `[gate] STOP #n: PERSON AHEAD … detect->gate 30 us` |
| Impact > 2.5 g or tilt > 30° (IMU) | brake, held 1 s | `[gate] STOP: IMU IMPACT/TILT …` |
| Jetson silent 200 ms (frozen, crashed, cable cut) | brake, recover automatically when commands return | `[fault] Jetson silent for 210 ms … safe state` / `[fault] Jetson link restored …` |
| Camera frames stop 500 ms (ribbon, sensor) | car held until frames return | `[gate] camera monitor LOST -> holding the car` |
| A task writes gatekeeper memory (MPU) | write blocked, car held 3 s | `[gate] STOP: MPU blocked a write … by task 6` |

## Switches and buttons

| Control | Function |
|---|---|
| BOOT0 / BOOT1 | left/left = boot from flash (normal). BOOT1 right = development mode |
| RESET (black) | restarts the firmware (≈ 5 s to camera, keep the board still for the IMU calibration) |
| **USER1 (blue)** | **fault injection**: the vision task deliberately writes into the gatekeeper's private memory. The MPU blocks it and the car is held for 3 s. Demonstrates spatial isolation between the AI task and the safety task |

## LCD

Live camera image with an overlay:
- title line "Guardian-TRON uT-Kernel 3.0 + NPU",
- a box around each detected person with its confidence ("person 97%"): **green** while clear, **red** while the car is held for a person,
- a red banner "STOP : PERSON AHEAD" during a person hazard,
- bottom line: NPU inference time, frame rate and the number of stops since boot.

## Console output (ST-LINK VCP, 115200 8N1)

| Prefix | Period | Meaning |
|---|---|---|
| `[rtos]` | boot | task set: priorities, releases, deadlines, criticality, failure handling |
| `[perf] context switch` | boot | 2000-sample context-switch benchmark |
| `[gate] n=… ok=… veto=… bad=… replay=… maxlat=…` | 1 s | gatekeeper counters since boot; `STOPPED`/`DRIVING`; camera state; UART overruns |
| `[vision] 15.0 fps \| NPU 28515 us \| persons 1 (tallest h 42%) \| clear/HAZARD` | 1 s | vision pipeline: frame rate, NPU time, detections, hazard state, `isp_err` = camera I2C errors |
| `[imu] roll pitch yaw a_h \| 100 Hz period … jitter …` | 5 s | IMU attitude (Kalman), monitor period and jitter, step time |
| `[perf] cpu idle … log … gate … imu … vision … report …` | 5 s | CPU share per task (from the dispatcher hook) and task switches per second |
| `[metric] name: n mean p50 p95 p99 max \| target < … over N -> PASS/FAIL` | 10 s | cumulative timing statistics since boot against the design targets (printed by the `report` task, priority 25, so reporting never delays a safety task) |
| `[stack] gate 1012/4096 B (24%) imu 1164/4096 B (28%) …` | 10 s | stack high-water mark per task (5 tasks, `report` is the tightest at 69 %) |
| `[gate] STOP …`, `[gate] SLOW …`, `[fault] …` | event | safety events with their latency breakdown |

## Tasks

| Task | Priority | Released by | Deadline | If it fails |
|---|---|---|---|---|
| `gate` | 8 | Jetson command (UART interrupt → event flag), hazard kick | 1 ms | 200 ms without a valid command → brake |
| `imu` | 10 | cyclic handler every 10 ms | 10 ms | tilt / impact → brake |
| `vision` | 20 | camera frame (≈ 15 fps) | 100 ms | no frame for 500 ms → car held |
| `report` | 25 | every 1 s | best effort | a late report, nothing else |
| `log` | 30 | ring buffer not empty | best effort | drops lines and counts them |

The boot log prints this table as `[rtos]` lines.

## Errors and recovery

| Symptom | Cause | Recovery |
|---|---|---|
| No log at all | terminal opened on the wrong port, or the ST-LINK VCP was opened before a USB reset | reopen the port; press RESET with the terminal open |
| `[vesc] NO REPLY` | VESC not powered or not wired | normal on a desk kit; on the car check the battery and the COMM cable (38400 baud) |
| `camera monitor LOST` stays | camera ribbon loose | power off, reseat the FFC at both ends, power on |
| `isp_err` increasing, fps < 15 | camera I2C disturbed (ribbon) | reseat the ribbon, power-cycle |
| `[imu] calibration: car moving, retrying` | board moved during the first second | keep it still; it retries every second |
| LCD black, no log after flashing | wrong image layout (unsigned or not `--align`ed) or NOR left in OPI mode by the loader | re-flash with `make sign` + `tools/flash_boot.sh`, then a full power-cycle |
| `DUPLICATE PROCESSES` in `run_demo.sh` | an older ROS 2 stack still running on the Jetson | `./run_demo.sh` stops it; or `pkill -f guardian` on the Jetson |
| `/dev/ttyUSB0` missing on the Jetson | PL2303 hung (`error -32` in dmesg) | unplug/replug the USB-TTL |
