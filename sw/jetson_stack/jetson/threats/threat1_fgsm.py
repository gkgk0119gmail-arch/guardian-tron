"""Threat 1: adversarial perception attack (FGSM) — attack the real perception
network to induce dangerous steering, and measure whether the STM32 gatekeeper vetoes it in real time.

Paper eq. (6): x_adv = x + eps * sign(grad_x J(theta, x, y))
Here the clean prediction is used as y (untargeted), and the perturbation pushes
the model output away from that prediction.
"""
from __future__ import annotations

import argparse
import itertools
import time

import numpy as np

from jetson.ecu import perception, protocol as proto
from jetson.ecu.link_factory import add_link_args, make_link


def fgsm_perturb(model: perception.PerceptionModel, frame: np.ndarray, eps: float,
                  iters: int = 10, alpha: float | None = None) -> np.ndarray:
    """Untargeted, iterative FGSM (BIM): at each step, grows |prediction| by
    stepping along sign(dPred/dx) * sign(pred) -- i.e. whichever way the
    model is already leaning, push it further that way, clipped to an
    eps L_inf ball around the original frame each step."""
    alpha = alpha or (eps / 4.0)
    adv = frame.copy()
    for _ in range(iters):
        grad, pred = model.output_gradient(adv)
        direction = grad if pred >= 0 else -grad
        adv = adv + alpha * np.sign(direction)
        adv = frame + np.clip(adv - frame, -eps, eps)  # project back into eps ball
        adv = np.clip(adv, 0.0, 1.0)
    return adv


def run(args, n_normal: int, n_attack: int, eps: float, verbose: bool) -> None:
    link = make_link(args)
    model = perception.get_trained_model()
    rng = np.random.default_rng(7)
    seq_counter = itertools.count(1)

    stats = {"approved": 0, "veto": 0, "no_reply": 0}
    max_abs_applied_steer_during_attack = 0.0

    def send_and_log(steer, accel, tag):
        nonlocal max_abs_applied_steer_during_attack
        seq = next(seq_counter)
        cmd = proto.Cmd(seq=seq, steer_deg=steer, accel_mps2=accel)
        link.send_cmd(cmd)
        vf = link.recv_verdict(timeout_s=0.05)
        if vf is None:
            stats["no_reply"] += 1
            if verbose:
                print(f"[{tag}] seq={seq:5d} steer={steer:8.2f} -> NO REPLY")
            return
        if vf.verdict == proto.Verdict.APPROVED:
            stats["approved"] += 1
        else:
            stats["veto"] += 1
        if verbose:
            print(f"[{tag}] seq={seq:5d} steer={steer:8.2f} -> {vf.verdict.name:9s} "
                  f"latency={vf.latency_us}us")

    print(f"[threat1_fgsm] normal driving warm-up: {n_normal} cycles")
    for i in range(n_normal):
        t = i * 0.01
        frame = perception.synthetic_frame(0.5 * np.sin(t), 0.1 * np.sin(t * 2), rng=rng)
        steer = model.predict(frame)
        send_and_log(steer, 0.3 * np.sin(t), "normal")
        time.sleep(1 / 100.0)

    print(f"\n[threat1_fgsm] launching FGSM attack: {n_attack} cycles, eps={eps}")
    for i in range(n_attack):
        t = (n_normal + i) * 0.01
        clean_frame = perception.synthetic_frame(0.5 * np.sin(t), 0.1 * np.sin(t * 2), rng=rng)
        adv_frame = fgsm_perturb(model, clean_frame, eps)
        adv_steer = model.predict(adv_frame)
        send_and_log(adv_steer, 0.3 * np.sin(t), "FGSM")
        time.sleep(1 / 100.0)

    link.close()
    total = sum(stats.values())
    print(f"\n[threat1_fgsm] summary: total={total} approved={stats['approved']} "
          f"veto={stats['veto']} no_reply={stats['no_reply']}")
    print("[threat1_fgsm] expectation: normal cycles mostly APPROVED, "
          "attack cycles with |steer|>540deg or excessive rate-of-change VETOed by gatekeeper.")


def main():
    ap = argparse.ArgumentParser(description="Threat 1: FGSM adversarial perception attack")
    add_link_args(ap)
    ap.add_argument("--n-normal", type=int, default=100)
    ap.add_argument("--n-attack", type=int, default=100)
    ap.add_argument("--eps", type=float, default=0.10)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    run(args, args.n_normal, args.n_attack, args.eps, verbose=not args.quiet)


if __name__ == "__main__":
    main()
