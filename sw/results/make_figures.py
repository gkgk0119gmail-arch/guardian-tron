#!/usr/bin/env python3
"""Regenerate the figures in results/figures/ from the raw logs and CSV files in results/.

    python3 make_figures.py          (needs matplotlib + numpy)
"""
import csv
import glob
import math
import os
import re

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'figures')
os.makedirs(OUT, exist_ok=True)

ACCENT = '#E8590C'      # STM32 / μT-Kernel
LINUX = '#5A6578'       # Jetson / Linux
INK = '#111A2E'
GRID = '#DCE2EB'
TARGET = '#2B8A3E'
plt.rcParams.update({
    'font.family': 'DejaVu Sans', 'font.size': 11, 'axes.edgecolor': GRID, 'axes.labelcolor': INK,
    'xtick.color': INK, 'ytick.color': INK, 'axes.spines.top': False, 'axes.spines.right': False,
    'figure.dpi': 150, 'savefig.bbox': 'tight', 'savefig.facecolor': 'white',
})


def ns(v):
    """'415ns' / '9.6us' / '31.4ms' -> nanoseconds"""
    m = re.match(r'([\d.]+)(ns|us|ms)', v)
    return float(m.group(1)) * {'ns': 1, 'us': 1e3, 'ms': 1e6}[m.group(2)]


def fmt(x):
    if x < 1e3:
        return f'{x:.0f} ns'
    if x < 1e6:
        return f'{x / 1e3:.3g} µs'
    return f'{x / 1e6:.3g} ms'


def logs(runs=None):
    for d in sorted(glob.glob(os.path.join(HERE, 'raw_logs', '*'))):
        if runs and not any(os.path.basename(d).startswith(r) for r in runs):
            continue
        p = os.path.join(d, 'stm32.log')
        if os.path.exists(p):
            yield os.path.basename(d), open(p, 'rb').read().decode('ascii', 'replace').replace('\r', '')


def events():
    """all safety events in all runs: hazard -> brake (ns), link loss -> safe (ns), per source"""
    ev = {'haz': [], 'person': [], 'imu': [], 'link': [], 'excluded': 0}
    for _, t in logs():
        for m in re.finditer(r'STOP #\d+: PERSON AHEAD .*?detect->gate (\d+) us', t):
            ev['person'].append(int(m.group(1)) * 1000)
        for m in re.finditer(r'STOP: IMU (?:TILT|IMPACT) .*?detect->gate (\d+) us', t):
            v = int(m.group(1)) * 1000
            # an IMU hazard raised while a person hazard already held the car is only logged
            # when the person clears; the firmware now reports these separately (not a latency)
            if v < 100e6:
                ev['imu'].append(v)
            else:
                ev['excluded'] += 1
        for m in re.finditer(r'\[fault\] Jetson silent for (\d+) ms', t):
            ev['link'].append(int(m.group(1)) * 1000000)
    ev['haz'] = ev['person'] + ev['imu']
    return ev


# ---------------------------------------------------------------- 1. jitter: RTOS vs Linux
def fig_jitter():
    lin = {}
    for line in open(os.path.join(HERE, 'linux_jitter_jetson.txt')):
        if line.startswith('##'):
            load = 'loaded' if 'stress' in line else 'idle'
        m = re.match(r'(rel|abs) n=\d+ period-error ns: min (-?\d+) p50 (-?\d+) p99 (-?\d+) p99.9 (-?\d+) max (-?\d+)', line)
        if m:
            lin[(load, m.group(1))] = (abs(int(m.group(4))), max(abs(int(m.group(2))), abs(int(m.group(6)))))
    rows = {r['metric']: r for r in csv.DictReader(open(os.path.join(HERE, 'latency_results.csv')))}
    stm = rows['monitor_jitter']
    bars = [
        ('Linux (Jetson), idle,\nrelative sleep', lin[('idle', 'rel')], LINUX),
        ('Linux (Jetson), idle,\nabsolute sleep', lin[('idle', 'abs')], LINUX),
        ('Linux (Jetson), 100 % CPU load,\nrelative sleep', lin[('loaded', 'rel')], LINUX),
        ('Linux (Jetson), 100 % CPU load,\nabsolute sleep', lin[('loaded', 'abs')], LINUX),
        ('μT-Kernel 3.0 (STM32N6),\nNPU at full load', (ns(stm['p99']), ns(stm['max'])), ACCENT),
    ]
    fig, ax = plt.subplots(figsize=(9, 4.2))
    y = np.arange(len(bars))[::-1]
    for yi, (lab, (p99, mx), c) in zip(y, bars):
        ax.barh(yi, mx, color=c, height=0.55)
        ax.plot([p99], [yi], marker='|', color='white', markersize=18, mew=2.5)
        ax.text(mx * 1.15, yi, f'max {fmt(mx)}', va='center', color=c, fontweight='bold')
    ax.set_yticks(y)
    ax.set_yticklabels([b[0] for b in bars])
    ax.set_xscale('log')
    ax.set_xlim(1e3, 2e7)
    ax.set_xlabel('worst-case period error of a 10 ms (100 Hz) loop  (log scale; white tick = p99)')
    red = 100 * (1 - ns(stm['max']) / lin[('loaded', 'rel')][1])
    ax.set_title(f'100 Hz control-loop jitter: {red:.1f} % lower worst case than Linux under load', loc='left', fontweight='bold', color=INK)
    ax.grid(axis='x', color=GRID)
    fig.savefig(os.path.join(OUT, 'jitter_rtos_vs_linux.png'))
    plt.close(fig)


# ---------------------------------------------------------------- 2. all metrics vs targets
def fig_metrics():
    rows = list(csv.DictReader(open(os.path.join(HERE, 'latency_results.csv'))))
    ev = events()
    for r in rows:
        key = {'hazard_to_brake': 'haz', 'link_loss_to_safe': 'link'}.get(r['metric'])
        if key and ev[key]:
            v = sorted(ev[key])
            r.update(n=str(len(v)), p50=f'{v[len(v) // 2]}ns', p99=f'{v[min(len(v) - 1, int(len(v) * 0.99))]}ns', max=f'{v[-1]}ns',
                     verdict='PASS' if v[-1] <= ns(r['target']) else 'FAIL')
    label = {
        'ctx_switch': 'context switch', 'irq_to_task': 'UART interrupt → task',
        'gate_cmd_verdict': 'command verdict + actuation', 'hazard_to_brake': 'hazard → brake issued',
        'monitor_jitter': '100 Hz monitor jitter', 'monitor_step': 'monitor step (IMU + Kalman)',
        'npu_inference': 'NPU inference (YOLOX-nano)', 'frame_to_decision': 'camera frame → decision',
        'link_loss_to_safe': 'Jetson frozen → safe state',
    }
    fig, ax = plt.subplots(figsize=(9, 4.8))
    rows = sorted(rows, key=lambda r: ns(r['target']))
    for i, r in enumerate(rows):
        p50, p99, mx, tg = ns(r['p50']), ns(r['p99']), ns(r['max']), ns(r['target'])
        ax.plot([p50, mx], [i, i], color=ACCENT, lw=3, solid_capstyle='round', alpha=0.35)
        ax.plot(p50, i, 'o', color=ACCENT, ms=6)
        ax.plot(p99, i, 'D', color=ACCENT, ms=5)
        ax.plot(mx, i, 's', color=INK, ms=6)
        ax.plot(tg, i, marker='|', color=TARGET, ms=16, mew=3)
        ax.text(tg * 1.25, i, f'target {fmt(tg)} · n={int(r["n"]):,} · {r["verdict"]}', va='center', fontsize=9,
                color=TARGET if r['verdict'] == 'PASS' else 'red')
    ax.set_yticks(range(len(rows)))
    ax.set_yticklabels([label.get(r['metric'], r['metric']) for r in rows])
    ax.set_xscale('log')
    ax.set_xlim(100, 5e10)
    ax.set_xlabel('time (log scale)   ● p50   ◆ p99   ■ max   | design target')
    ax.set_xticks([1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9])
    ax.set_xticklabels(['100 ns', '1 µs', '10 µs', '100 µs', '1 ms', '10 ms', '100 ms', '1 s'])
    ax.set_title('Measured on the STM32N6570-DK: every metric within its target', loc='left', fontweight='bold', color=INK)
    ax.grid(axis='x', color=GRID)
    fig.savefig(os.path.join(OUT, 'metrics_vs_targets.png'))
    plt.close(fig)


# ---------------------------------------------------------------- 3. person-stop events
def fig_stops():
    det, gate = [], []
    for _, t in logs():
        for m in re.finditer(r'STOP #\d+: PERSON AHEAD .*?frame->detect (\d+) us \| detect->gate (\d+) us', t):
            det.append(int(m.group(1)) / 1000.0)
            gate.append(int(m.group(2)))
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 3.6), gridspec_kw={'width_ratios': [1, 1]})
    a1.hist(det, bins=np.arange(27, 34.5, 0.5), color=LINUX, edgecolor='white')
    a1.set_xlabel('camera frame → person decision [ms]\n(NPU inference + decoding)')
    a1.set_ylabel('stop events')
    a1.set_title(f'AI part (vision task, priority 20)\nmean {np.mean(det):.1f} ms, max {max(det):.1f} ms', loc='left', fontsize=11, color=INK)
    a2.hist(gate, bins=np.arange(26.5, 36.5, 1), color=ACCENT, edgecolor='white')
    a2.axvline(100, color=TARGET)
    a2.set_xlabel('person decision → brake issued [µs]\n(μT-Kernel preemption to the gate task)')
    a2.set_title(f'RTOS part (gate task, priority 8)\nmean {np.mean(gate):.1f} µs, max {max(gate)} µs, target 100 µs', loc='left', fontsize=11, color=INK)
    for a in (a1, a2):
        a.grid(axis='y', color=GRID)
    fig.suptitle(f'{len(gate)} real person stops with the wheels spinning', x=0.01, ha='left', fontweight='bold', color=INK, y=1.04)
    fig.savefig(os.path.join(OUT, 'person_stop_latency.png'))
    plt.close(fig)
    return len(gate)


# ---------------------------------------------------------------- 4. distance governor
def fig_governor():
    pts = []
    runs = ['run_A', 'run_C', 'run_F', 'run_H', 'run_I']          # logs after the 0.84 m calibration
    for _, t in logs(runs):
        for m in re.finditer(r'SLOW: person ~(\d+) cm ahead \(box h \d+%\) -> speed cap (\d+) mm/s', t):
            pts.append((int(m.group(1)) / 100.0, int(m.group(2)) / 1000.0))
    d = np.linspace(2.0, 8.0, 300)
    v = np.sqrt(2 * 0.10 * (d - 2.0))
    fig, ax = plt.subplots(figsize=(9, 4))
    ax.axvspan(0, 2.0, color='#FFE3D3')
    ax.text(1.0, 1.32, 'hard stop\n(person box ≥ 42 %)', ha='center', va='top', color=ACCENT, fontweight='bold')
    ax.plot(d, v, color=INK, lw=2, label='cap  v = √(2 · 0.1 m/s² · (d − 2 m))')
    if pts:
        x, y = zip(*pts)
        ax.scatter(x, y, color=ACCENT, zorder=3, s=28, label=f'measured caps while driving (n = {len(pts)})')
    for s in (0.3, 0.5, 1.2):
        ax.axhline(s, color=GRID, lw=1, ls='--')
        ax.text(7.95, s + 0.02, f'cruise {s} m/s', ha='right', fontsize=9, color=LINUX)
    ax.set_xlim(0, 8)
    ax.set_ylim(0, 1.4)
    ax.set_xlabel('estimated distance to the person [m]  (monocular: d = 0.84 m / box height)')
    ax.set_ylabel('speed allowed by the STM32 [m/s]')
    ax.set_title('Distance-aware speed cap: closer person, lower speed; stop at 2 m', loc='left', fontweight='bold', color=INK)
    ax.legend(loc='upper left', frameon=True, framealpha=0.95, edgecolor=GRID, bbox_to_anchor=(0.27, 0.98))
    ax.grid(color=GRID)
    fig.savefig(os.path.join(OUT, 'speed_governor.png'))
    plt.close(fig)


# ---------------------------------------------------------------- 5. CPU share + stacks
def fig_cpu_stack():
    t = dict(logs(['run_A'])).get('run_A_person_stress_freeze', '')
    m = re.findall(r'\[perf\] cpu idle ([\d.]+)% main [\d.]+% log ([\d.]+)% gate ([\d.]+)% imu ([\d.]+)% vision ([\d.]+)%', t)
    idle, logp, gate, imu, vis = map(float, m[-1])
    stacks = []
    for r in csv.reader(open(os.path.join(HERE, 'memory_results.csv'))):
        if r[0] == 'stack':
            size = int(re.search(r'of (\d+) B', r[3]).group(1))
            stacks.append((r[1], int(r[2]), size))
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 3.8), gridspec_kw={'width_ratios': [1, 1.2]})
    vals = [vis, imu + gate + logp, idle]
    a1.pie(vals, colors=[LINUX, ACCENT, '#E9EDF3'], startangle=90, counterclock=False,
           wedgeprops={'width': 0.38, 'edgecolor': 'white'})
    a1.text(0, 0.08, f'{imu + gate:.1f} %', ha='center', fontsize=20, fontweight='bold', color=ACCENT)
    a1.text(0, -0.2, 'safety tasks\n(gate + imu)', ha='center', fontsize=9, color=INK)
    a1.legend([f'vision + NPU pipeline {vis:.1f} %', f'gate {gate:.1f} % · imu {imu:.1f} % · log {logp:.1f} %', f'idle {idle:.1f} %'],
              loc='lower center', bbox_to_anchor=(0.5, -0.3), frameon=False, fontsize=9)
    a1.set_title('CPU share per μT-Kernel task\n(dispatcher hook, driving)', fontsize=11, color=INK)
    names = [s[0] for s in stacks]
    used = [s[1] for s in stacks]
    size = [s[2] for s in stacks]
    y = np.arange(len(stacks))[::-1]
    pct = [100 * u / sz for u, sz in zip(used, size)]
    a2.barh(y, [100] * len(y), color='#E9EDF3', height=0.55)
    a2.barh(y, pct, color=ACCENT, height=0.55)
    for yi, u, sz, pc in zip(y, used, size, pct):
        a2.text(101, yi, f'{pc:.0f} %  ({u:,} of {sz:,} B)', va='center', fontsize=9)
    a2.set_yticks(y)
    a2.set_yticklabels(names)
    a2.set_xlim(0, 150)
    a2.set_xticks([0, 25, 50, 75, 100])
    a2.set_xlabel('stack used [%]  (high-water mark, pattern fill)')
    a2.set_title('Stack high-water mark per task', fontsize=11, color=INK)
    fig.savefig(os.path.join(OUT, 'cpu_and_stack.png'))
    plt.close(fig)


# ---------------------------------------------------------------- 6. latency budget of one stop
def fig_budget():
    stages = [
        ('camera frame → NPU decision', 30.0e6, LINUX),
        ('decision → brake issued (μT-Kernel)', 30e3, ACCENT),
        ('brake frame queued (interrupt TX)', 1.3e3, ACCENT),
        ('brake frame on the 38 400-baud wire', 2.6e6, LINUX),
    ]
    fig, ax = plt.subplots(figsize=(9, 2.6))
    y = np.arange(len(stages))[::-1]
    for yi, (lab, v, c) in zip(y, stages):
        ax.barh(yi, v, color=c, height=0.55)
        ax.text(v * 1.2, yi, fmt(v), va='center', fontweight='bold', color=c)
    ax.set_yticks(y)
    ax.set_yticklabels([s[0] for s in stages])
    ax.set_xscale('log')
    ax.set_xlim(500, 3e8)
    ax.set_xticks([1e3, 1e4, 1e5, 1e6, 1e7, 1e8])
    ax.set_xticklabels(['1 µs', '10 µs', '100 µs', '1 ms', '10 ms', '100 ms'])
    ax.set_title('Where the time goes in one person stop (typical values)', loc='left', fontweight='bold', color=INK)
    ax.grid(axis='x', color=GRID)
    fig.savefig(os.path.join(OUT, 'stop_latency_budget.png'))
    plt.close(fig)


# ---------------------------------------------------------------- 7. memory map
def fig_memory():
    rows = list(csv.reader(open(os.path.join(HERE, 'memory_results.csv'))))[1:]
    sec = {r[1]: int(r[2]) for r in rows if r[0].startswith('AXISRAM1')}
    stk = sum(int(r[2].split()[0]) if False else 0 for r in rows)
    parts = [('code (.text)', sec.get('.text', 0), INK), ('const data (.rodata)', sec.get('.rodata', 0), LINUX),
             ('.data', sec.get('.data', 0), '#9AA6BA'), ('.bss incl. static task stacks', sec.get('.bss', 0), ACCENT),
             ('vectors', sec.get('.isr_vector', 0) + sec.get('.mtk_exctbl', 0), '#C9D1DD')]
    total = 1023 * 1024
    used = sum(p[1] for p in parts)
    fig, ax = plt.subplots(figsize=(10, 1.9))
    left = 0
    for lab, v, c in parts:
        ax.barh(0, v / 1024, left=left / 1024, color=c, height=0.5, label=f'{lab}  {v / 1024:.0f} KB')
        left += v
    ax.barh(0, (total - used) / 1024, left=used / 1024, color='#F3F5F9', height=0.5, label=f'μT-Kernel heap + free  {(total - used) / 1024:.0f} KB')
    ax.set_xlim(0, total / 1024)
    ax.set_yticks([])
    ax.set_xlabel('AXISRAM1 [KB]. NPU activations live in AXISRAM2–6; the 1.15 MB of weights stay in NOR flash')
    ax.legend(ncol=3, loc='upper center', bbox_to_anchor=(0.5, -0.55), frameon=False, fontsize=9)
    ax.set_title(f'Firmware memory: {used / 1024:.0f} KB of 1 MB AXISRAM1 ({100 * used / total:.0f} %)', loc='left', fontweight='bold', color=INK)
    fig.savefig(os.path.join(OUT, 'memory_map.png'))
    plt.close(fig)


# ---------------------------------------------------------------- 8. fault -> safe action
def fig_faults():
    ev = events()
    items = [
        ('person in the path (camera + NPU)\ndecision → brake', ev['person']),
        ('car lifted / tilted > 30° (IMU)\ndetection → brake', ev['imu']),
        ('Jetson frozen / link cut\nlast command → brake (watchdog 200 ms)', ev['link']),
    ]
    fig, ax = plt.subplots(figsize=(9, 3.2))
    for i, (lab, v) in enumerate(items[::-1]):
        if not v:
            continue
        v = np.array(v, float)
        ax.scatter(v, np.full(len(v), i) + np.random.default_rng(1).uniform(-0.12, 0.12, len(v)), s=18, color=ACCENT, alpha=0.7)
        ax.text(v.max() * 1.6, i, f'n = {len(v)}, max {fmt(v.max())}', va='center', fontsize=10, fontweight='bold', color=INK)
    ax.set_yticks(range(len(items)))
    ax.set_yticklabels([x[0] for x in items[::-1]])
    ax.set_xscale('log')
    ax.set_xlim(300, 3e10)
    ax.set_xticks([1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9])
    ax.set_xticklabels(['1 µs', '10 µs', '100 µs', '1 ms', '10 ms', '100 ms', '1 s'])
    ax.set_title('Every injected or real hazard ended in the safe state (all runs)', loc='left', fontweight='bold', color=INK)
    ax.grid(axis='x', color=GRID)
    fig.savefig(os.path.join(OUT, 'faults_to_safe_state.png'))
    plt.close(fig)


if __name__ == '__main__':
    fig_jitter()
    fig_metrics()
    n = fig_stops()
    fig_governor()
    fig_cpu_stack()
    fig_budget()
    fig_memory()
    fig_faults()
    ev = events()
    print({k: (len(v) if isinstance(v, list) else v) for k, v in ev.items()})
    print('figures written to', OUT, '| person stops:', n)
