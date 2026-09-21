"""Main AI ECU loop, runs on the Jetson.

Every control cycle: render/capture a frame -> perception model predicts
steering -> combine with a simple cruise-accel rule -> send CMD frame to the
STM32 gatekeeper over the safety-critical link -> read back the VERDICT and
log it. This mirrors the gatekeeper firmware operation flow in the paper (Ⅳ.2).

Transport is UDP over Ethernet by default, matching the actual physical link
between this Jetson and the STM32N6570-DK (RJ45, not USB/UART) and the
paper's own raw-UDP safety link design (port 5005). Pass --transport uart to
use a serial link instead if the wiring changes.
"""
from __future__ import annotations

import argparse
import itertools
import time

import numpy as np

from . import perception, protocol as proto
from .link_factory import add_link_args, make_link

CONTROL_HZ = 100.0


def cruise_accel(t: float) -> float:
    """Gentle, bounded accel profile so a clean run stays inside the
    envelope by construction (0.4 m/s^2 sinusoidal cruise)."""
    return 0.4 * np.sin(t * 0.5)


def run(args) -> None:
    link = make_link(args)
    model = perception.get_trained_model()
    rng = np.random.default_rng(42)

    period = 1.0 / CONTROL_HZ
    t0 = time.monotonic()
    seq_counter = itertools.count(1)

    n_approved = n_veto = n_no_reply = 0

    try:
        while time.monotonic() - t0 < args.duration:
            cycle_start = time.monotonic()
            t = cycle_start - t0

            curvature = 0.6 * np.sin(t * 0.3)
            offset = 0.1 * np.sin(t * 0.9)
            frame = perception.synthetic_frame(curvature, offset, rng=rng)
            steer = model.predict(frame)
            accel = cruise_accel(t)

            seq = next(seq_counter)
            cmd = proto.Cmd(seq=seq, steer_deg=steer, accel_mps2=accel)
            link.send_cmd(cmd)

            vf = link.recv_verdict(timeout_s=0.05)
            if vf is None:
                n_no_reply += 1
                if not args.quiet:
                    print(f"seq={seq:6d} NO REPLY (link/timeout)")
            else:
                if vf.verdict == proto.Verdict.APPROVED:
                    n_approved += 1
                else:
                    n_veto += 1
                if not args.quiet:
                    print(f"seq={seq:6d} steer={steer:7.2f} accel={accel:6.2f} "
                          f"-> {vf.verdict.name:9s} latency={vf.latency_us}us")

            elapsed = time.monotonic() - cycle_start
            time.sleep(max(0.0, period - elapsed))
    except KeyboardInterrupt:
        pass
    finally:
        link.close()
        total = n_approved + n_veto + n_no_reply
        print(f"\n[main_ecu] done. total={total} approved={n_approved} "
              f"veto={n_veto} no_reply={n_no_reply}")


def main():
    ap = argparse.ArgumentParser(description="Jetson main AI ECU (perception + gatekeeper client)")
    add_link_args(ap)
    ap.add_argument("--duration", type=float, default=15.0, help="seconds to run")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    run(args)


if __name__ == "__main__":
    main()
