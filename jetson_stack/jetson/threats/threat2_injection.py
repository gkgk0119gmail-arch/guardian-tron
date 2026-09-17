"""위협② 차내 네트워크 인젝션/스푸핑 유사 공격.

실제 CAN 버스가 아니라 Jetson<->STM32 안전 임계 UDP 링크(실제 배선은
이더넷)에 대해, 메인 ECU 프로세스를 거치지 않고 다음을 직접 주입한다:
  (a) 위험한 절댓값/변화율을 가진 위조 CMD 프레임 (스푸핑)
  (b) 이미 사용된 낮은 seq 번호의 재전송 (리플레이)
  (c) CRC/프레이밍이 깨진 임의 바이트열 (퍼징/DoS 성격)
게이트키퍼는 안전포락선 위반, seq 역행, CRC 실패를 각각 독립적으로 방어해야
한다 (gatekeeper_core.c 의 n_veto / n_malformed / n_replay_or_reorder 집계).
"""
from __future__ import annotations

import argparse
import random
import time

from jetson.ecu import protocol as proto
from jetson.ecu.link_factory import add_link_args, make_link


def spoofed_dangerous_cmd(seq: int) -> proto.Cmd:
    return proto.Cmd(seq=seq, steer_deg=random.choice([700.0, -900.0, 5000.0]),
                      accel_mps2=random.choice([-50.0, 20.0]))


def run(args, n_each: int, verbose: bool) -> None:
    link = make_link(args)
    seq = 1
    counts = {"approved": 0, "veto": 0, "malformed": 0, "no_reply": 0}

    def expect(vf, label):
        if vf is None:
            counts["no_reply"] += 1
            if verbose:
                print(f"[{label}] NO REPLY")
            return
        counts[vf.verdict.name.lower()] += 1
        if verbose:
            print(f"[{label}] seq={vf.seq:5d} -> {vf.verdict.name}")

    print(f"[threat2_injection] (a) spoofed out-of-envelope frames x{n_each}")
    for _ in range(n_each):
        cmd = spoofed_dangerous_cmd(seq)
        link.send_cmd(cmd)
        expect(link.recv_verdict(0.05), "spoof")
        seq += 1
        time.sleep(0.005)

    print(f"\n[threat2_injection] (b) replay of a stale seq x{n_each}")
    stale = proto.Cmd(seq=1, steer_deg=5.0, accel_mps2=0.2)  # seq already consumed above
    for _ in range(n_each):
        link.send_cmd(stale)
        expect(link.recv_verdict(0.05), "replay")
        time.sleep(0.005)

    print(f"\n[threat2_injection] (c) malformed/corrupted raw bytes x{n_each}")
    for _ in range(n_each):
        junk = bytes(random.randint(0, 255) for _ in range(proto.CMD_FRAME_LEN))
        link.send_raw(junk)
        expect(link.recv_verdict(0.05), "junk")
        time.sleep(0.005)

    link.close()
    total = sum(counts.values())
    print(f"\n[threat2_injection] summary: total={total} {counts}")
    print("[threat2_injection] expectation: (a) all VETO, (b) all VETO or flagged as "
          "replay internally, (c) all MALFORMED -- none should reach the actuator.")


def main():
    ap = argparse.ArgumentParser(description="Threat 2: spoofed/replayed/corrupted link injection")
    add_link_args(ap)
    ap.add_argument("--n-each", type=int, default=50)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    run(args, args.n_each, verbose=not args.quiet)


if __name__ == "__main__":
    main()
