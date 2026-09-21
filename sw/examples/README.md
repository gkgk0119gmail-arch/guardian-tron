# Examples

`hibiki_eval.py` checks the project's claims from what the firmware prints on its console, and prints a PASS / FAIL / NOT RUN table.

```bash
pip install -r ../requirements.txt                          # pyserial (+ matplotlib/numpy for the graphs)

python3 hibiki_eval.py --offline ../results/raw_logs/run_A_person_stress_freeze/stm32.log   # no hardware
python3 hibiki_eval.py                                      # the board on USB: guided checks
python3 hibiki_eval.py --host COM5                          # + USB-TTL: the PC plays the Jetson and injects faults
python3 hibiki_eval.py --list-ports
```

| Check | What is judged | Source line on the console |
|---|---|---|
| E1–E7 | μT-Kernel boot, context switch, interrupt → task, 100 Hz monitor jitter and step, stacks, CPU share | `microT-Kernel Version`, `[metric]`, `[stack]`, `[perf] cpu` |
| A1–A4 | NPU pipeline rate, frame → decision, person → brake, distance-based speed cap | `[vision]`, `[gate] STOP #n: PERSON AHEAD`, `[gate] SLOW:` |
| F1–F7 | IMU tilt/impact stop, MPU-blocked write, Jetson link loss, command verdict, unsafe / replayed / corrupted commands | `[gate] STOP: IMU`, `[gate] STOP: MPU`, `[fault]`, `[gate] n=… veto= bad= replay=` |

Live runs save `out/<date_time>/report.md` and the raw `console.log`.
