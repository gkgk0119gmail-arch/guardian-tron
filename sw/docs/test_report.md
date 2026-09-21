# Test Report

All results were measured on the HIBIKI implementation itself (STM32N6570-DK + μT-Kernel 3.0, this repository's firmware) on 2026-09-21. Design targets come from the project proposal. Measured results are kept separate from them. Raw logs: `sw/results/raw_logs/`. Tables: `sw/results/latency_results.csv`, `sw/results/memory_results.csv`, `sw/results/linux_jitter_jetson.txt`.

## 1. Measurement environment

| Item | Setting |
|---|---|
| Board | STM32N6570-DK, Cortex-M55 at 800 MHz, Neural-ART NPU, MB1854 camera module |
| RTOS | μT-Kernel 3.0 BSP2, 1 ms tick, tasks gate (8) / imu (10) / vision (20) / log (30) |
| Load on the STM32 | camera + YOLOX-nano on the NPU at 15 fps (continuous), LCD overlay, 100 Hz IMU monitor, 100 Hz Jetson commands |
| Load on the Jetson (run A) | `stress --cpu 6 --io 2 --vm 2 --vm-bytes 256M` (all 6 cores at 100 %) during the whole run, plus the ROS 2 stack |
| Vehicle | F1TENTH car on a stand (wheels spinning, 0.5 m/s command), VESC 6 MkVI |
| Timebase | DWT cycle counter (1.25 ns). Statistics in the firmware: log-linear histogram, 32 buckets per octave (±1.5 % percentile error); min/max exact |
| Pass/fail | per metric: max ≤ target (strictest reading). "over" = number of samples above the target |

## 2. Timing results (run A: person hazards + Jetson at 100 % CPU + Jetson freeze; run B: link loss)

| Metric | What exactly | Target | n | mean | p50 | p95 | p99 | **max** | Verdict |
|---|---|---|---|---|---|---|---|---|---|
| Context switch | `tk_wup_tsk()` → higher-priority task running (boot benchmark) | < 5.7 µs | 2000 | 415 ns | 412 ns | 412 ns | 412 ns | **878 ns** | PASS |
| Interrupt → task | USART2 RX interrupt → gatekeeper task running | < 50 µs | 72 257 | 512 ns | 468 ns | 648 ns | 1.17 µs | **6.1 µs** | PASS |
| Command verdict | one Jetson command: CRC, replay, envelope, VESC update, verdict TX queued | < 1 ms | 4 538 | 4.7 µs | 4.3 µs | 8.6 µs | 9.6 µs | **15.5 µs** | PASS |
| **Hazard → brake** | vision (or IMU) raises the hazard → gate task issues the brake frame (all runs: 57 person stops + 5 IMU tilts) | < 100 µs | 62 | 27.7 µs | 30 µs | 33 µs | 33.4 µs | **34 µs** | PASS |
| Frame → decision | camera frame in RAM → NPU inference + decoding + decision | < 100 ms | 1 487 | 30.0 ms | 30.1 ms | 31.1 ms | 31.1 ms | **31.4 ms** | PASS |
| NPU inference | YOLOX-nano 480×480 INT8 | < 50 ms | 1 487 | 28.5 ms | 28.5 ms | 28.5 ms | 28.5 ms | **29.2 ms** | PASS |
| Monitor jitter | \|period − 10 ms\| of the 100 Hz IMU monitor | < 100 µs | 9 783 | 318 ns | 138 ns | 1.0 µs | 2.3 µs | **23.2 µs** | PASS |
| Monitor step (WCET, observed) | I2C read + Kalman update + decision | < 1 ms | 9 784 | 323 µs | 324 µs | 324 µs | 324 µs | **401 µs** | PASS |
| Link loss → safe state | last valid Jetson command → brake (Jetson frozen or stack stopped, all runs) | < 250 ms | 7 | 208 ms | 210 ms | 210 ms | 210 ms | **210 ms** | PASS |

Deadline misses (monitor finished after its next release): **0**. UART overruns: **0**.
One IMU tilt from an early run was logged 1.18 s late because the car was already held by a person hazard at that moment. That is not a reaction latency. It is excluded here, and the current firmware reports such events separately (`IMU … while already stopped`).
Graphs of all of the above: `sw/results/figures/` (regenerate with `sw/results/make_figures.py`).
The brake frame itself takes 2.6 ms on the 38400-baud VESC wire after it is issued (a physical limit of the A5 pin, see hw/wiring.md).
"WCET" here is the observed maximum over the stated n, not a static or probabilistic bound.

## 3. RTOS vs Linux: the same 100 Hz periodic loop

`sw/jetson_stack/bench/jitter_probe.c` on the Jetson Orin Nano (L4T R36.4.7), 2 000 periods per line, two independent sessions:

| Condition | Session 1: max period error | Session 2: max period error |
|---|---|---|
| Jetson idle, relative sleep | 224 µs | 308 µs |
| Jetson idle, absolute sleep | 221 µs | 306 µs |
| Jetson loaded, relative sleep (typical control loop) | **3 888 µs** | **3 337 µs** |
| Jetson loaded, absolute sleep (best case for a normal thread) | 445 µs | 235 µs |
| **STM32 μT-Kernel monitor, NPU at full load (run A, n = 9 783)** | **23.2 µs** | |

Worst-case jitter reduction versus Linux under load: **99.3 %** (typical relative-sleep loop, 3 337 µs → 23.2 µs, the more favourable Linux session) and **90 %** (Linux best case, 235 µs → 23.2 µs).
On the Jetson, the host-side round-trip time of a command through the USB-TTL is 10 ms on average and up to 32 ms. The STM32 verdict itself takes at most 15.5 µs.

## 4. CPU and memory

| Item | Measured |
|---|---|
| CPU share (dispatcher hook, 5 s windows, run A) | vision 47.8 %, imu 3.2 %, gate 0.2 %, log < 0.1 %, idle 48.7 % |
| Safety-task overhead (gate + imu) | **3.4 %** (target < 3 %: not met. The IMU is read by polling I2C at 400 kHz. An interrupt-driven read conflicted with the camera's use of the same bus, so we kept polling) |
| Task switches | ≈ 2 600 /s while driving |
| Stack high-water mark | gate 1 384 / 4 096 B (33 %), imu 1 008 / 4 096 B (24 %), vision 1 508 / 32 768 B (4 %), log 436 / 1 024 B (42 %) |
| AXISRAM1 (1 MB) | code 151 KB, read-only data 468 KB, data 54 KB, bss 61 KB (includes the 41 KB of static task stacks), kernel heap for the rest |
| PSRAM | 332 KB post-processing scratch, 2.2 MB camera/LCD buffers |
| NOR flash | firmware image 659 KB, NPU weights 1.15 MB |

## 5. Functional and fault-injection results

| Test | Method | Result | Log |
|---|---|---|---|
| Person stop, wheels spinning | person walks in within ≈ 2 m (real person, 6–12 times per run) | stop every time, `detect->gate` 29–31 µs, resume 1.5 s after the person leaves | run A, run C |
| Distance-graded slowdown | person walks in from ≈ 4 m | cap 0.44 m/s at ≈ 3 m, 0.31 m/s at ≈ 2.5 m, 0.22 m/s at ≈ 2.3 m, stop at ≈ 2 m | run C |
| Jetson at 100 % CPU | `run_demo.sh --stress` | all STM32 metrics PASS, unchanged from the unloaded runs | run A |
| Jetson frozen 1.5 s | `run_demo.sh --freeze` (SIGSTOP/SIGCONT on the bridge) | safe state after 210 ms, automatic recovery when commands return | run B |
| AI task writes gatekeeper memory | USER1 button (fault injection in the vision task) | MPU blocks the store (MemManage), the car is held 3 s, the system keeps running | run D |
| Car lifted / tilted | tilt > 30° | stop, `detect->gate` 1 µs | run D |
| Out-of-envelope command | `pc_host.py --scenario unsafe` (40° steering) | VETO, clamped to 17.5° | console |
| Replayed commands | `pc_host.py --scenario replay` | rejected (replay counter) | console |
| Corrupted frames | `pc_host.py --scenario garbage` | MALFORMED, resynchronised, normal frames accepted right after | console |
| Camera lost | camera stalls / ribbon out | car held (`camera monitor LOST`) until frames return | earlier runs |

## 6. Defects found by these measurements and fixed

1. **The T-Monitor console masks interrupts while it prints.** `tm_putstring()` polls USART1 inside `DI()`, so a 200-character status line blocked every interrupt (the kernel tick too) and the printing task for ≈ 17 ms. Monitor jitter was ±16 ms. Fix: `tm_printf`/`tm_putstring` are wrapped at link time into a RAM ring buffer, drained by the lowest-priority `log` task through the USART1 TX FIFO (`os/gv_log.c`).
2. **Polling UART transmission in the highest-priority task.** Each VESC frame (2.6 ms at 38400 baud) was busy-waited. Jitter was ±4.2 ms. Fix: interrupt-driven TX ring (`gt/gt_uart.c`). The brake command is now queued in ≈ 1.3 µs.
3. **10 ms kernel tick.** `HAL_Delay()` in the camera path rounded up to 10 ms. With a 1 ms tick the vision rate went from 10 to 15 fps.
4. **Shared I2C bus without locking.** The camera sensor and the IMU share I2C1. A μT-Kernel mutex with priority inheritance (`TA_INHERIT`) now serialises them. The camera driver's calls are wrapped at link time (`os/gv_i2c_lock.c`).
5. **Link-loss detection only while driving.** A frozen Jetson went unnoticed while the car was already held for a person. The watchdog now watches valid frames regardless of the car state.
6. **Boot from flash stayed silent.** The signing tool (CubeProgrammer ≥ 2.21) put a 0x240-byte header in front of the image, but the FSBL expects the application at 0x400. `--align` fixes it.

## 7. Not measured / limitations

- **Rare monitor jitter spikes in long runs: fixed.** The four floor runs recorded while filming (121 034 periods) had 20 periods (0.017 %) starting 0.1–0.82 ms late and a 1.14 ms longest step. The cause was the gate task (priority 8) formatting the 10 s metrics report itself. Moving the reports to a `report` task at priority 25 removed it: `run_J` (80 932 periods, Jetson commanding at 100 Hz) and a following 23.5 min run (141 054 periods) show **0 periods over 100 µs, max 12.5 µs, longest step 434 µs**. The flashed image and `sw/binaries/` contain the fix.

- Power consumption was not measured.
- The distance estimate is monocular (box height). Calibrated with one real person: box height 42 % at 2.0 m. It depends on the person's height and the camera pitch.
- The detector is ST's pre-trained model. It detects real people reliably but did not detect a mannequin we tried.
- External cross-check with a logic analyzer: the probe pins exist (D2/D3/D4), but no logic-analyzer capture is included.
