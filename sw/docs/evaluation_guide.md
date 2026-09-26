# Evaluation Guide

What to run, what to look at, and what counts as a pass. The fastest path is the runbook in the [README](../../README.md): one terminal watches the STM32 over USB (`python3 sw/examples/hibiki_eval.py --watch`), a second one drives the car from the Jetson over ssh (`~/guardian/hibiki.sh start 0.3` → `arm`). That script judges every check below automatically and writes `sw/examples/out/<date_time>/report.md`.

Four ways to run the checks:

| How | Command | Covers |
|---|---|---|
| The car (normal) | `hibiki_eval.py --watch` on the laptop + `hibiki.sh` on the Jetson | everything except the four command-injection faults |
| The board alone on a desk | `hibiki_eval.py` (guided prompts) | RTOS metrics, person stop, MPU, IMU |
| Board + a 3.3 V USB-TTL instead of the Jetson | `hibiki_eval.py --host <port>` | adds unsafe / replayed / corrupted commands and the watchdog |
| No hardware at all | `hibiki_eval.py --offline sw/results/raw_logs/<run>/stm32.log` | re-judges our recorded runs |

The checks below are the same ones the script applies; each row names the console line it is judged from, so they can also be read by eye in any serial terminal (115200 8N1 on the ST-LINK port).

Setup for the manual route: BOOT0 and BOOT1 both left, ST-LINK USB-C to the PC, terminal open from power-on so the boot lines are captured.

## Mode DEMO_NORMAL: μT-Kernel 3.0 and the task set

| # | Do | Expect | Pass if |
|---|---|---|---|
| E1 | Power on | `microT-Kernel Version 3.00`, then the `[rtos]` task table (gate 8, imu 10, vision 20, report 25, log 30) | both lines present |
| E2 | Wait 10 s | `[metric] ctx_switch: n=2000 mean 444ns p50 460ns p95 460ns p99 460ns max 872ns | target < 5700ns over 0 -> PASS` | max < 5.7 µs (we measured 0.87 µs) |
| E3 | Watch 30 s | `[vision] 15.0 fps \| NPU 28… us`, `isp_err 0`; LCD shows the camera | 14–15 fps, NPU < 50 ms |
| E4 | Watch the `[perf] cpu` line | CPU share per task, e.g. `idle 47 % gate 0.2 % imu 3.2 % vision 49 %` | gate + imu ≈ 3.4 % while the NPU runs |
| E5 | Watch the `[metric] monitor_jitter` line | 100 Hz safety monitor jitter while the NPU runs at full load | max < 100 µs (we measured ≤ 11.9 µs) |
| E6 | Watch the `[stack]` line | stack high-water mark per task | all < 80 % |

## Mode DEMO_HAZARD: AI result → real-time safety action

| # | Do | Expect | Pass if |
|---|---|---|---|
| E7 | Walk into the camera view within about 2 m (the model detects real people, not mannequins) | LCD: box turns red, banner "STOP : PERSON AHEAD". Log: `[gate] STOP #n: PERSON AHEAD (conf …, h …) \| frame->detect ~30000 us \| detect->gate 30 us \| brake frame queued in ~1300 ns` | `detect->gate` < 100 µs |
| E8 | Step out of view | after 1.5 s: `[gate] hazard clear -> accepting Jetson commands again` | line present |
| E9 | **[host]** `python3 sw/tools/pc_host.py --port <port> --speed 0.5 --secs 40`, then walk slowly toward the camera from about 4 m | `[gate] SLOW: person ~300 cm ahead … speed cap 4xx mm/s`, the cap falls as you approach, then `STOP` | the cap decreases with distance |
| E10 | Tilt the board more than 30° for 0.1 s (lift the car) | `[gate] STOP: IMU TILT (-37 deg) \| … detect->gate 1 us` | line present |
| E11 | Tap the board sharply (> 2.5 g sideways) | `[gate] STOP: IMU IMPACT (…)` | line present (may need a firm tap) |

## Mode DEMO_FAULT: faults the STM32 must survive

| # | Do | Expect | Pass if |
|---|---|---|---|
| E12 | Press the blue **USER1** button (AI task writes gatekeeper memory) | `[vision] FAULT INJECTION: writing into gatekeeper memory …`, `[gate] STOP: MPU blocked a write to gatekeeper memory by task 6 …`, the system keeps running, commands accepted again after 3 s | write blocked, no crash |
| E13 | **[host]** `pc_host.py … --scenario freeze` (host stalls 1.5 s) | `[fault] Jetson silent for 2xx ms (watchdog 200 ms) -> safe state`, then `[fault] Jetson link restored after … ms` | safe state < 250 ms after the last frame, automatic recovery |
| E14 | **[host]** `--scenario unsafe` (40° steering) | host prints `VETO=` rising; STM32 clamps to 17.5° | VETO count > 0 |
| E15 | **[host]** `--scenario replay` (old sequence numbers) | STM32 `replay=` counter rises | counter > 0 |
| E16 | **[host]** `--scenario garbage` (broken CRC + noise) | `bad=` rises, then normal frames are accepted again (resync) | APPROVED continues after the fault |
| E17 | Pull the camera ribbon (power off first!) and power on | `[gate] camera monitor LOST -> holding the car`, the car is never released | line present, no `DRIVING` |
| E18 | **[car]** `sw/run_demo.sh 0.5 60 --stress --freeze` | same `[metric]` results with the Jetson at 100 % CPU; freeze → safe state → recovery | all `[metric]` lines PASS |

## Mode DEMO_FAULT on Linux, for comparison (optional, any Linux PC)

`gcc -O2 -o jitter_probe sw/jetson_stack/bench/jitter_probe.c && ./jitter_probe abs 20`, then again under `stress --cpu $(nproc) --io 2 --vm 2`.
On our Jetson Orin Nano: max period error 221 µs idle and 445 µs (absolute sleep) or 3 888 µs (relative sleep) under load. The STM32 monitor with the NPU at full load and the Jetson commanding at 100 Hz: 11.9 µs over 80 932 periods.

## The summary table

After a few minutes the firmware prints, every 10 s. This is the last block of our
reference run, copied unchanged from
`sw/results/raw_logs/run_J_long_after_report_fix/stm32.log` (13 min, the same firmware
that is flashed on the board):

```
[metric] ctx_switch: n=2000 mean 444ns p50 460ns p95 460ns p99 460ns max 872ns | target < 5700ns over 0 -> PASS
[metric] irq_to_task: n=695600 mean 546ns p50 520ns p95 680ns p99 1104ns max 9501ns | target < 50.0us over 0 -> PASS
[metric] gate_cmd_verdict: n=43119 mean 5479ns p50 4416ns p95 8576ns p99 8832ns max 13.4us | target < 1000.0us over 0 -> PASS
[metric] hazard_to_brake: n=4 mean 30.3us p50 30.4us p95 30.7us p99 30.7us max 30.7us | target < 100.0us over 0 -> PASS
[metric] frame_to_decision: n=12160 mean 29.8ms p50 29.6ms p95 30.6ms p99 30.6ms max 31.4ms | target < 100.0ms over 0 -> PASS
[metric] npu_inference: n=12160 mean 28.5ms p50 28.5ms p95 28.5ms p99 28.5ms max 28.7ms | target < 50.0ms over 0 -> PASS
[metric] monitor_jitter: n=80932 mean 235ns p50 101ns p95 840ns p99 1968ns max 11.9us | target < 100.0us over 0 -> PASS
[metric] monitor_step: n=80933 mean 322.8us p50 323.5us p95 323.5us p99 323.5us max 422.1us | target < 1000.0us over 0 -> PASS
[metric] link_loss_to_safe: no samples yet
```

Each counter only covers what happened in that one run: `run_J` had 4 person stops and
nobody froze the Jetson, so `link_loss_to_safe` stayed empty. Pooled over every committed
run, `hazard_to_brake` is n = 66, max 34 µs (61 person stops + 5 IMU tilts) and
`link_loss_to_safe` is n = 7, max 210 ms. Those pooled numbers are what
`sw/results/latency_results.csv`, the graphs and the test report quote; `make_figures.py`
recomputes them from the logs.

`irq_to_task`, `gate_cmd_verdict` and `link_loss_to_safe` need a host sending commands (the Jetson on the car, or `pc_host.py` on a USB-TTL). `hazard_to_brake` needs a person or an IMU event. Our reference runs are in `sw/results/`.
