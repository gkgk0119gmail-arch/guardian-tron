"""위협③ OTA 펌웨어 침해 — 신뢰루트(RoT) 시뮬레이션.

실기판에서는 STM32N6 부트 ROM이 FSBL 서명 헤더를 검증하지만, CubeProgrammer
없이는 실제 보안 부팅 파이프라인을 여기서 재현할 수 없다 (docs/STM32_PORT.md
참조). 대신 그 검증 로직(서명 부착 -> 검증 -> 부정 이미지 로드 거부)을 실제
Ed25519 서명으로 그대로 구현해서, "OEM 서명 키가 없는 공격자가 변조한 펌웨어는
부팅 전 단계에서 거부된다"는 신뢰루트의 핵심 보장을 실측 검증한다.

키는 최초 실행 시 jetson/threats/keys/ 에 생성되어 재사용된다. 이 디렉터리의
oem_signing_key.pem 은 "OEM 서명 키"에 해당하며 실배포에서는 게이트키퍼
빌드 파이프라인 밖(HSM 등)에 있어야 한다 -- 데모 편의상 로컬에 둔 것뿐이다.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey,
)
from cryptography.hazmat.primitives import serialization
from cryptography.exceptions import InvalidSignature

KEY_DIR = Path(__file__).parent / "keys"
PRIV_PATH = KEY_DIR / "oem_signing_key.pem"
PUB_PATH = KEY_DIR / "oem_signing_key.pub"

HEADER_MAGIC = b"FSBL"
HEADER_VERSION = 2  # mirrors paper's "헤더 버전 2.3" in spirit, simplified here


def ensure_keys() -> tuple[Ed25519PrivateKey, Ed25519PublicKey]:
    KEY_DIR.mkdir(parents=True, exist_ok=True)
    if PRIV_PATH.exists():
        priv = serialization.load_pem_private_key(PRIV_PATH.read_bytes(), password=None)
    else:
        priv = Ed25519PrivateKey.generate()
        PRIV_PATH.write_bytes(priv.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption(),
        ))
        pub_bytes = priv.public_key().public_bytes(
            encoding=serialization.Encoding.Raw, format=serialization.PublicFormat.Raw)
        PUB_PATH.write_bytes(pub_bytes)
        print(f"[rot] generated new OEM signing keypair at {KEY_DIR}")
    return priv, priv.public_key()


def sign_image(priv: Ed25519PrivateKey, image: bytes) -> bytes:
    """Returns header(magic+version+len) + signature + image, the layout the
    'boot ROM' (verify_and_load) expects."""
    digest = hashlib.sha256(image).digest()
    sig = priv.sign(digest)
    header = HEADER_MAGIC + bytes([HEADER_VERSION]) + len(image).to_bytes(4, "little")
    return header + sig + image


def verify_and_load(pub: Ed25519PublicKey, signed_blob: bytes) -> tuple[bool, str]:
    """Stand-in for the STM32 boot ROM's signature check before FSBL
    execution. Returns (accepted, reason)."""
    if len(signed_blob) < 9 + 64:
        return False, "truncated header"
    magic = signed_blob[0:4]
    version = signed_blob[4]
    length = int.from_bytes(signed_blob[5:9], "little")
    sig = signed_blob[9:9 + 64]
    image = signed_blob[9 + 64:]

    if magic != HEADER_MAGIC:
        return False, "bad magic"
    if version != HEADER_VERSION:
        return False, "unsupported header version"
    if length != len(image):
        return False, "length mismatch"

    digest = hashlib.sha256(image).digest()
    try:
        pub.verify(sig, digest)
    except InvalidSignature:
        return False, "signature verification failed"
    return True, "ok"


def run(fw_path: str | None, verbose: bool) -> None:
    priv, pub = ensure_keys()

    if fw_path:
        image = Path(fw_path).read_bytes()
    else:
        image = b"GATEKEEPER_FIRMWARE_PLACEHOLDER_v1 " + b"\x00" * 64
        print("[threat3_ota_tamper] no --firmware given, using a placeholder image "
              "(pass --firmware gatekeeper/build/gatekeeper_sim for a real binary)")

    print(f"\n[threat3_ota_tamper] (1) legitimate OEM-signed image, {len(image)} bytes")
    good_blob = sign_image(priv, image)
    ok, reason = verify_and_load(pub, good_blob)
    print(f"  verify_and_load -> accepted={ok} ({reason})")
    assert ok, "legitimate signed image was rejected -- RoT implementation bug"

    print("\n[threat3_ota_tamper] (2) attacker flips one byte in the signed image "
          "(no access to OEM private key)")
    tampered = bytearray(good_blob)
    flip_at = 9 + 64 + (len(image) // 2)
    tampered[flip_at] ^= 0xFF
    ok, reason = verify_and_load(pub, bytes(tampered))
    print(f"  verify_and_load -> accepted={ok} ({reason})")
    assert not ok, "tampered image was accepted -- RoT implementation bug"

    print("\n[threat3_ota_tamper] (3) attacker self-signs with a forged key "
          "(simulating a compromised/unauthorized signer)")
    forged_priv = Ed25519PrivateKey.generate()
    forged_blob = sign_image(forged_priv, image)
    ok, reason = verify_and_load(pub, forged_blob)  # verified against the real OEM pubkey
    print(f"  verify_and_load -> accepted={ok} ({reason})")
    assert not ok, "forged-key image was accepted -- RoT implementation bug"

    print("\n[threat3_ota_tamper] all three cases behaved as expected: "
          "only the genuine OEM-signed image loads.")


def main():
    ap = argparse.ArgumentParser(description="Threat 3: OTA / firmware supply-chain tamper vs RoT")
    ap.add_argument("--firmware", default=None, help="path to a real firmware/binary image")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    run(args.firmware, verbose=not args.quiet)


if __name__ == "__main__":
    main()
