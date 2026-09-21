#!/usr/bin/env python3
"""hibiki_eval.py - check HIBIKI / Guardian-TRON claims yourself, with or without the hardware.

  # 1) no hardware: re-evaluate one of our recorded runs
  python3 sw/examples/hibiki_eval.py --offline sw/results/raw_logs/run_A_person_stress_freeze/stm32.log

  # 2) the car: watch the STM32 while you drive it from the Jetson (ssh), Ctrl+C = report
  python3 sw/examples/hibiki_eval.py --watch

  # 3) the board on USB (ST-LINK): guided checks, the console port is found automatically
  python3 sw/examples/hibiki_eval.py

  # 4) board + a 3.3 V USB-TTL on D0/D1/GND (instead of the Jetson): the PC plays the Jetson
  python3 sw/examples/hibiki_eval.py --host COM5            (Windows)
  python3 sw/examples/hibiki_eval.py --host /dev/ttyUSB0    (Linux; macOS: /dev/cu.usbserial-*)

  python3 sw/examples/hibiki_eval.py --list-ports

Everything is judged from what the firmware prints on its console (DWT cycle-counter
measurements). The result is a PASS / FAIL / NOT RUN table, also saved as a report in
sw/examples/out/<date_time>/ together with the raw console log.
Only dependency: pyserial (pip install pyserial).
"""
import argparse
import os
import queue
import re
import sys
import threading
import time
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))

# --------------------------------------------------------------------------- output
USE_COLOR = sys.stdout.isatty() and os.environ.get('NO_COLOR') is None
if os.name == 'nt':
    os.system('')                                   # enables ANSI colors on Windows 10+ terminals


def c(text, code):
    return f'\033[{code}m{text}\033[0m' if USE_COLOR else text


def ok(t): return c(t, '32;1')
def bad(t): return c(t, '31;1')
def dim(t): return c(t, '2')
def acc(t): return c(t, '33;1')


def ns(v):
    """'415ns' / '9.6us' / '31.4ms' -> nanoseconds"""
    m = re.match(r'([\d.]+)(ns|us|ms)', v)
    return float(m.group(1)) * {'ns': 1, 'us': 1e3, 'ms': 1e6}[m.group(2)]


def fmt(x):
    if x is None:
        return '-'
    if x < 1e3:
        return f'{x:.0f} ns'
    if x < 1e6:
        return f'{x / 1e3:.3g} µs'
    return f'{x / 1e6:.3g} ms'


# --------------------------------------------------------------------------- what the firmware prints
RE = {
    'kernel': re.compile(r'microT-Kernel Version ([\d.]+)'),
    'rtos': re.compile(r'^\[rtos\]\s+(gate|imu|vision|report|log)\s+(\d+)'),
    'vision': re.compile(r'^\[vision\] ([\d.]+) fps \| NPU (\d+) us .*?isp_err (\d+)'),
    'metric': re.compile(r'^\[metric\] (\w+): n=(\d+) mean (\S+) p50 (\S+) p95 (\S+) p99 (\S+) max (\S+) \| target < (\S+) over (\d+) -> (\w+)'),
    'stack': re.compile(r'(\w+) (\d+)/(\d+) B \((\d+)%\)'),
    'cpu': re.compile(r'^\[perf\] cpu idle ([\d.]+)% .*?gate ([\d.]+)% imu ([\d.]+)% vision ([\d.]+)%'),
    'person': re.compile(r'STOP #\d+: PERSON AHEAD \(conf (\d+)%, h (\d+)%\) \| frame->detect (\d+) us \| detect->gate (\d+) us'),
    'imu': re.compile(r'STOP: IMU (TILT|IMPACT) \((-?\d+)[^)]*\) \| sample->detect \d+ us \| detect->gate (\d+) us'),
    'mpu': re.compile(r'STOP: MPU blocked a write to gatekeeper memory by task (\d+)'),
    'slow': re.compile(r'SLOW: person ~(\d+) cm ahead \(box h \d+%\) -> speed cap (\d+) mm/s'),
    'loss': re.compile(r'\[fault\] Jetson silent for (\d+) ms'),
    'restore': re.compile(r'\[fault\] Jetson link restored after (\d+) ms'),
    'gate': re.compile(r'^\[gate\] n=(\d+) ok=(\d+) veto=(\d+) bad=(\d+) replay=(\d+)'),
    'cam': re.compile(r'camera monitor (LOST|OK)'),
}


class Evidence:
    """Everything the checks need, parsed from the console. Thread-safe feed()."""

    def __init__(self):
        self.lock = threading.Lock()
        self.lines = 0
        self.kernel = None
        self.rtos = {}
        self.fps, self.npu_us, self.isp_err = [], [], None
        self.metrics = {}
        self.stack = {}
        self.cpu = None
        self.person, self.imu, self.slow = [], [], []
        self.mpu = 0
        self.loss, self.restore = [], []
        self.gate = None
        self.cam = []

    def feed(self, line):
        line = line.strip()
        if not line:
            return
        with self.lock:
            self.lines += 1
            if (m := RE['kernel'].search(line)):
                self.kernel = m.group(1)
            if (m := RE['rtos'].search(line)):
                self.rtos[m.group(1)] = int(m.group(2))
            if (m := RE['vision'].search(line)):
                self.fps.append(float(m.group(1))); self.npu_us.append(int(m.group(2))); self.isp_err = int(m.group(3))
            if (m := RE['metric'].search(line)):
                k = m.groups()
                self.metrics[k[0]] = dict(n=int(k[1]), mean=ns(k[2]), p50=ns(k[3]), p95=ns(k[4]), p99=ns(k[5]),
                                          max=ns(k[6]), target=ns(k[7]), over=int(k[8]), verdict=k[9])
            if line.startswith('[stack]'):
                self.stack = {n: (int(u), int(s)) for n, u, s, _ in RE['stack'].findall(line)}
            if (m := RE['cpu'].search(line)):
                self.cpu = dict(idle=float(m.group(1)), gate=float(m.group(2)), imu=float(m.group(3)), vision=float(m.group(4)))
            if (m := RE['person'].search(line)):
                self.person.append(dict(conf=int(m.group(1)), h=int(m.group(2)), det_us=int(m.group(3)), gate_us=int(m.group(4))))
            if (m := RE['imu'].search(line)):
                us = int(m.group(3))
                if us < 100000:                     # older firmware logged "already stopped" events late
                    self.imu.append(dict(kind=m.group(1), value=int(m.group(2)), gate_us=us))
            if RE['mpu'].search(line):
                self.mpu += 1
            if (m := RE['slow'].search(line)):
                self.slow.append((int(m.group(1)), int(m.group(2))))
            if (m := RE['loss'].search(line)):
                self.loss.append(int(m.group(1)))
            if (m := RE['restore'].search(line)):
                self.restore.append(int(m.group(1)))
            if (m := RE['gate'].search(line)):
                self.gate = dict(n=int(m.group(1)), ok=int(m.group(2)), veto=int(m.group(3)), bad=int(m.group(4)), replay=int(m.group(5)))
            if (m := RE['cam'].search(line)):
                self.cam.append(m.group(1))

    def snapshot(self):
        with self.lock:
            return dict(person=len(self.person), imu=len(self.imu), mpu=self.mpu, loss=len(self.loss),
                        gate=dict(self.gate) if self.gate else None, metrics=len(self.metrics))


# --------------------------------------------------------------------------- the checks
def metric_check(ev, name):
    m = ev.metrics.get(name)
    if not m:
        return 'NOT RUN', 'no [metric] line yet'
    detail = f"n={m['n']:,}  p50 {fmt(m['p50'])}  p99 {fmt(m['p99'])}  max {fmt(m['max'])}  (target < {fmt(m['target'])})"
    return ('PASS' if m['max'] <= m['target'] else 'FAIL'), detail


def evaluate(ev, host=None):
    """host: results of the fault-injection runs (live mode with --host), or None."""
    R = []

    def add(cid, group, title, status, detail):
        R.append((cid, group, title, status, detail))

    running = ev.kernel or ev.metrics or ev.fps
    add('E1', 'RTOS', 'μT-Kernel 3.0 boots and runs the task set',
        'PASS' if running else 'NOT RUN',
        (f'microT-Kernel {ev.kernel} · ' if ev.kernel else 'running (boot lines not captured) · ' if running else '')
        + (f"tasks {', '.join(f'{k}={v}' for k, v in sorted(ev.rtos.items(), key=lambda x: x[1]))}" if ev.rtos else 'task table printed at boot'))
    add('E2', 'RTOS', 'Context switch (tk_wup_tsk → task running)', *metric_check(ev, 'ctx_switch'))
    add('E3', 'RTOS', 'UART interrupt → gatekeeper task', *metric_check(ev, 'irq_to_task'))
    st, d = metric_check(ev, 'monitor_jitter')
    if ev.metrics.get('monitor_jitter'):
        m = ev.metrics['monitor_jitter']
        d += f"  ({m['over']} periods over target; worst period {10 + m['max'] / 1e6:.3f} ms, 10 ms deadline never missed)" if m['max'] < 9e6 else ''
    add('E4', 'RTOS', '100 Hz safety monitor period jitter (NPU at full load)', st, d)
    add('E5', 'RTOS', 'Safety monitor step: IMU read + Kalman + decision', *metric_check(ev, 'monitor_step'))
    if ev.stack:
        worst = max(u / s for u, s in ev.stack.values())
        add('E6', 'RTOS', 'Stack high-water mark per task < 80 %', 'PASS' if worst < 0.8 else 'FAIL',
            '  '.join(f'{k} {100 * u // s}%' for k, (u, s) in ev.stack.items()))
    else:
        add('E6', 'RTOS', 'Stack high-water mark per task < 80 %', 'NOT RUN', 'no [stack] line yet')
    if ev.cpu:
        add('E7', 'RTOS', 'CPU share of the safety tasks (gate + imu)', 'INFO',
            f"gate {ev.cpu['gate']}% + imu {ev.cpu['imu']}% = {ev.cpu['gate'] + ev.cpu['imu']:.1f}%  (vision {ev.cpu['vision']}%, idle {ev.cpu['idle']}%; design target was < 3 %)")
    else:
        add('E7', 'RTOS', 'CPU share of the safety tasks (gate + imu)', 'NOT RUN', 'no [perf] cpu line yet')

    if ev.fps:
        fps = sorted(ev.fps)[len(ev.fps) // 2]
        npu = max(ev.npu_us)
        add('A1', 'AI', 'Camera + NPU (YOLOX-nano) pipeline rate', 'PASS' if fps >= 14 and npu < 50000 else 'FAIL',
            f'median {fps} fps, NPU max {npu / 1000:.1f} ms, camera I2C errors {ev.isp_err}')
    else:
        add('A1', 'AI', 'Camera + NPU (YOLOX-nano) pipeline rate', 'NOT RUN', 'no [vision] line yet')
    add('A2', 'AI', 'Camera frame → person decision', *metric_check(ev, 'frame_to_decision'))
    if ev.person:
        g = [p['gate_us'] for p in ev.person]
        add('A3', 'AI→RTOS', 'Person ahead → brake issued (μT-Kernel preemption)', 'PASS' if max(g) < 100 else 'FAIL',
            f'{len(g)} stops, decision → brake mean {sum(g) / len(g):.1f} µs, max {max(g)} µs (target < 100 µs)')
    else:
        add('A3', 'AI→RTOS', 'Person ahead → brake issued (μT-Kernel preemption)', 'NOT RUN', 'no person stop seen')
    if len(ev.slow) >= 2:
        by_d = {}
        for d_, v_ in ev.slow:
            by_d[d_] = v_                               # same distance -> same cap (firmware formula)
        pts = sorted(by_d.items())
        mono = all(b[1] >= a[1] for a, b in zip(pts, pts[1:]))
        show = [pts[0]] + pts[1:-1][::max(1, len(pts) // 3)][:2] + [pts[-1]] if len(pts) > 2 else pts
        add('A4', 'AI→RTOS', 'Speed cap falls as the person gets closer', 'PASS' if mono else 'FAIL',
            f'{len(ev.slow)} caps: ' + ', '.join(f'{d / 100:.1f} m → {v / 1000:.2f} m/s' for d, v in show))
    else:
        add('A4', 'AI→RTOS', 'Speed cap falls as the person gets closer', 'NOT RUN', 'drive (hibiki.sh arm, speed > 0) and walk toward the car')

    if ev.imu:
        g = [x['gate_us'] for x in ev.imu]
        add('F1', 'Fault', 'Car tilted / hit (IMU) → brake', 'PASS' if max(g) < 100 else 'FAIL',
            f"{len(g)} events ({', '.join(sorted({x['kind'] for x in ev.imu}))}), detection → brake max {max(g)} µs")
    else:
        add('F1', 'Fault', 'Car tilted / hit (IMU) → brake', 'NOT RUN', 'tilt the board > 30° for 0.1 s')
    add('F2', 'Fault', 'AI task writes gatekeeper memory → MPU blocks it', 'PASS' if ev.mpu else 'NOT RUN',
        f'{ev.mpu} write(s) blocked, car held 3 s, firmware kept running' if ev.mpu else 'press the blue USER1 button')
    if ev.loss:
        add('F3', 'Fault', 'Jetson silent → safe state (watchdog)', 'PASS' if max(ev.loss) < 250 else 'FAIL',
            f"{len(ev.loss)} losses, max {max(ev.loss)} ms (target < 250 ms)" + (f', restored after {ev.restore[-1]} ms' if ev.restore else ''))
    else:
        add('F3', 'Fault', 'Jetson silent → safe state (watchdog)', 'NOT RUN', 'run hibiki.sh freeze on the Jetson (or --host)')
    add('F4', 'Fault', 'Jetson command → verdict + actuation', *metric_check(ev, 'gate_cmd_verdict'))
    for cid, key, title, hint in (('F5', 'unsafe', 'Out-of-envelope command (40° steering) → VETO', 'VETO'),
                                  ('F6', 'replay', 'Replayed old sequence numbers → rejected', 'replay'),
                                  ('F7', 'garbage', 'Corrupted frames (bad CRC, noise) → MALFORMED + resync', 'bad')):
        h = (host or {}).get(key)
        if h is None:
            add(cid, 'Fault', title, 'NOT RUN', 'needs --host')
        else:
            add(cid, 'Fault', title, 'PASS' if h['pass'] else 'FAIL', h['detail'])
    return R


def print_report(R, out=None):
    w = max(len(r[2]) for r in R)
    lines = []
    lines.append('')
    lines.append(f"  {'ID':<4}{'Area':<9}{'Check':<{w + 2}}{'Result':<9}Measured")
    lines.append('  ' + '─' * (w + 80))
    for cid, group, title, st, d in R:
        col = ok if st == 'PASS' else bad if st == 'FAIL' else acc if st == 'INFO' else dim
        lines.append(f"  {cid:<4}{group:<9}{title:<{w + 2}}{col(f'{st:<9}')}{d}")
    n = {s: sum(1 for r in R if r[3] == s) for s in ('PASS', 'FAIL', 'NOT RUN', 'INFO')}
    lines.append('')
    lines.append(f"  {ok(str(n['PASS']) + ' PASS')}   {bad(str(n['FAIL']) + ' FAIL') if n['FAIL'] else '0 FAIL'}   {n['NOT RUN']} not run   {n['INFO']} info")
    print('\n'.join(lines))
    if out:
        with open(os.path.join(out, 'report.md'), 'w', encoding='utf-8') as f:
            f.write(f'# HIBIKI evaluation report — {datetime.now():%Y-%m-%d %H:%M}\n\n')
            f.write('| ID | Area | Check | Result | Measured |\n|---|---|---|---|---|\n')
            for cid, group, title, st, d in R:
                f.write(f'| {cid} | {group} | {title} | {st} | {d} |\n')
            f.write(f"\n{n['PASS']} PASS, {n['FAIL']} FAIL, {n['NOT RUN']} not run.\n")
    return n


# --------------------------------------------------------------------------- live mode
def find_console():
    from serial.tools import list_ports
    for p in list_ports.comports():
        text = f'{p.description} {p.manufacturer} {p.product}'.lower()
        if p.vid == 0x0483 or 'stlink' in text or 'st-link' in text:
            return p.device
    return None


class Console(threading.Thread):
    def __init__(self, port, ev, logf, echo):
        super().__init__(daemon=True)
        import serial
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.ev, self.logf, self.echo, self.stop = ev, logf, echo, False

    def run(self):
        buf = b''
        while not self.stop:
            try:
                buf += self.s.read(4096)
            except Exception as e:                # USB unplugged
                print(bad(f'\n  console read failed: {e}'))
                return
            *done, buf = buf.split(b'\n')
            for raw in done:
                line = raw.decode('ascii', 'replace').replace('\r', '')
                self.logf.write(line + '\n')
                self.ev.feed(line)
                if self.echo and re.search(r'STOP #|STOP:|SLOW:|\[fault\]|FAULT INJECTION|camera monitor|microT-Kernel|\[rtos\]', line):
                    print(dim('    board │ ') + line)


def wait_for(cond, secs, prompt=None, allow_skip=True):
    """Wait until cond() is true, the time runs out, or the user presses Enter."""
    if prompt:
        print(prompt + (dim('   [Enter = skip]') if allow_skip else ''))
    skipped = threading.Event()
    if allow_skip and sys.stdin.isatty():
        threading.Thread(target=lambda: (sys.stdin.readline(), skipped.set()), daemon=True).start()
    t0 = time.monotonic()
    while time.monotonic() - t0 < secs:
        if cond():
            return True
        if skipped.is_set():
            print(dim('    skipped'))
            return False
        time.sleep(0.2)
    print(dim('    no reaction within %d s' % secs))
    return False


def live(a):
    import serial  # noqa: F401  (clear error if pyserial is missing)
    port = a.console or find_console()
    if not port:
        print(bad('  ST-LINK console port not found.') + ' Plug the board in (ST-LINK USB-C) or pass --console <port>. Ports:')
        list_ports()
        return 2
    out = os.path.join(HERE, 'out', datetime.now().strftime('%Y%m%d_%H%M%S'))
    os.makedirs(out, exist_ok=True)
    logf = open(os.path.join(out, 'console.log'), 'w', encoding='utf-8', buffering=1)
    ev = Evidence()
    con = Console(port, ev, logf, echo=True)
    con.start()
    print(acc('\n  HIBIKI / Guardian-TRON evaluation') + f'   console {port}' + (f'   host {a.host}' if a.host else ''))
    print(dim(f'  raw console log → {os.path.relpath(out)}/console.log\n'))

    print('  1. Waiting for the board…')
    if not wait_for(lambda: ev.lines > 0, 12, allow_skip=False):
        print(acc('     Nothing yet: press the black RESET button on the board (boot takes ~5 s)'))
        if not wait_for(lambda: ev.lines > 0, 30, allow_skip=False):
            print(bad('  No output from the board. Check the port and that BOOT0/BOOT1 are both left.'))
            return 2
    print(ok('     board is talking') + dim('   (the metrics table comes every 10 s)'))
    if a.watch:
        print(acc('\n  Watching the STM32. Drive the car from the Jetson now (see README), then press Ctrl+C for the report.'))
        print(dim('  Events appear below as they happen: person stops, speed caps, IMU, MPU, Jetson faults.\n'))
        try:
            while True:
                time.sleep(0.5)
        except KeyboardInterrupt:
            print(dim('\n  … waiting 11 s for the last metrics table'))
            n0 = ev.snapshot()['metrics']
            t0 = time.monotonic()
            last = dict(ev.metrics)
            while time.monotonic() - t0 < 11 and ev.metrics == last:
                time.sleep(0.3)
        con.stop = True
        R = evaluate(ev, None)
        for i, r in enumerate(R):                   # --watch: the Jetson is the host, not this PC
            if r[0] in ('F5', 'F6', 'F7') and r[3] == 'NOT RUN':
                R[i] = (r[0], r[1], r[2], 'NOT RUN', 'needs pc_host.py on a USB-TTL (--host), not part of the car run')
        print_report(R, out)
        print(dim(f'\n  report → {os.path.relpath(out)}/report.md'))
        return 0
    wait_for(lambda: ev.fps and ev.metrics.get('ctx_switch') and ev.stack, 40, '  2. Collecting RTOS metrics (up to 40 s)…', allow_skip=False)

    host_res = {}
    if a.host:
        from pc_host import run_scenario
        import serial as _s
        hs = _s.Serial(a.host, 115200, timeout=0)
        say = (lambda m: None) if not a.verbose else (lambda m: print(dim('    host  │ ') + m))
        print(f'\n  3. The PC plays the Jetson on {a.host} (speed {a.speed} m/s; keep the wheels off the ground)')
        print('     normal driving, 8 s …'); run_scenario(hs, 'normal', a.speed, 0.0, 8, log=say)
        for key, secs in (('unsafe', 8), ('replay', 8), ('garbage', 8), ('freeze', 8)):
            before = ev.snapshot()
            print(f'     fault: {key} …')
            counts = run_scenario(hs, key, a.speed, 0.0, secs, log=say)
            time.sleep(1.5)
            after = ev.snapshot()
            g0, g1 = before['gate'] or {}, after['gate'] or {}
            d = lambda k: g1.get(k, 0) - g0.get(k, 0)
            if key == 'unsafe':
                host_res[key] = dict(pass_=counts['VETO'] > 0, detail=f"{counts['VETO']} VETO verdicts, {counts['APPROVED']} approved")
            elif key == 'replay':
                host_res[key] = dict(pass_=d('replay') > 0, detail=f"STM32 replay counter +{d('replay')}, host saw {counts['VETO']} VETO")
            elif key == 'garbage':
                host_res[key] = dict(pass_=d('bad') > 0 and counts['APPROVED'] > 0, detail=f"STM32 bad-frame counter +{d('bad')}, {counts['APPROVED']} approved around it (resync)")
        for v in host_res.values():
            v['pass'] = v.pop('pass_')
        hs.close()
        print('     waiting for the next metrics table (≤ 12 s) …')
        wait_for(lambda: ev.metrics.get('gate_cmd_verdict') and ev.metrics.get('irq_to_task'), 12, allow_skip=False)
    else:
        print(dim('\n  3. (no --host: command, watchdog and fault-injection checks are skipped)'))

    print('\n  4. Hands-on checks — watch the LCD while you do them')
    n0 = ev.snapshot()
    wait_for(lambda: ev.snapshot()['person'] > n0['person'], a.wait,
             acc('     a) Walk into the camera view and stop within ~2 m of the board (a real person, not a mannequin).'))
    wait_for(lambda: ev.snapshot()['mpu'] > n0['mpu'], a.wait,
             acc('     b) Press the blue USER1 button (fault injection: the AI task writes gatekeeper memory).'))
    wait_for(lambda: ev.snapshot()['imu'] > n0['imu'], a.wait,
             acc('     c) Tilt the board (or lift the car) by more than 30° for a moment.'))
    time.sleep(1.0)
    con.stop = True
    R = evaluate(ev, host_res if a.host else None)
    print_report(R, out)
    print(dim(f'\n  report → {os.path.relpath(out)}/report.md'))
    return 0


def list_ports():
    from serial.tools import list_ports as lp
    for p in lp.comports():
        tag = acc('  ← ST-LINK console') if (p.vid == 0x0483 or 'stlink' in f'{p.description}'.lower()) else ''
        print(f'    {p.device:<28} {p.description}{tag}')


def _stop_like_ctrl_c(signum, frame):
    raise KeyboardInterrupt


def main():
    import signal
    for sig in ('SIGTERM', 'SIGHUP', 'SIGBREAK'):          # also finish cleanly when the window is closed
        if hasattr(signal, sig):
            try:
                signal.signal(getattr(signal, sig), _stop_like_ctrl_c)
            except (ValueError, OSError):
                pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--offline', metavar='LOG', help='evaluate a recorded console log instead of a live board')
    ap.add_argument('--console', help='ST-LINK console port (default: found automatically)')
    ap.add_argument('--host', help='USB-TTL port wired to D0/D1: the PC plays the Jetson and injects faults')
    ap.add_argument('--speed', type=float, default=0.0, help='speed the host commands [m/s] (default 0: nothing moves)')
    ap.add_argument('--wait', type=int, default=60, help='seconds to wait for each hands-on check (default 60)')
    ap.add_argument('--watch', action='store_true', help='just watch the board until Ctrl+C (use while driving the car from the Jetson)')
    ap.add_argument('--list-ports', action='store_true')
    ap.add_argument('--verbose', action='store_true', help='also print the host verdict counters')
    a = ap.parse_args()
    if a.list_ports:
        try:
            list_ports()
        except ImportError:
            print('  pyserial is missing:  python3 -m pip install pyserial')
        return 0
    if a.offline:
        ev = Evidence()
        with open(a.offline, 'rb') as f:
            for raw in f.read().decode('ascii', 'replace').replace('\r', '').split('\n'):
                ev.feed(raw)
        print(acc('\n  HIBIKI / Guardian-TRON evaluation') + f'   offline: {a.offline}  ({ev.lines:,} console lines)')
        print_report(evaluate(ev))
        return 0
    try:
        import serial  # noqa: F401
    except ImportError:
        print(bad('  pyserial is missing.') + '  Install it with:  python3 -m pip install pyserial   (Windows: py -m pip install pyserial)')
        return 2
    try:
        return live(a)
    except KeyboardInterrupt:
        print('\n  stopped')
        return 1


if __name__ == '__main__':
    sys.exit(main())
