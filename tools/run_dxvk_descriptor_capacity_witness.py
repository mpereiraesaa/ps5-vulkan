#!/usr/bin/env python3
"""Run and strictly verify one bounded descriptor-set capacity witness."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_descriptor_capacity_witness import (  # noqa: E402
    DRAWS, IMAGES, PROFILES, STRIDE, source_word, texel, texel_word)
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

SENTINEL = 0xcdcdcdcd


def fnv1a(payload: bytes) -> int:
    digest = 2166136261
    for value in payload:
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    return digest


def expected_capacity() -> tuple[int, int, int]:
    compute = b"".join(struct.pack("<I", texel(k)) for k in range(IMAGES - 1))
    pixels = b"".join(struct.pack("<I", texel(k)) for k in range(IMAGES))
    texels = b"".join(struct.pack("<I", texel_word(k)) for k in range(IMAGES - 1))
    return fnv1a(compute), fnv1a(pixels), fnv1a(texels)


def expected_dynamic() -> tuple[int, str]:
    compute = b""
    for slot in range(2 * DRAWS):
        s, i = divmod(slot, DRAWS)
        compute += b"".join(struct.pack("<I", source_word(s, i, c)) for c in range(4))
        compute += struct.pack("<I", SENTINEL) * (STRIDE // 4 - 4)
    pixels = [b"".join(struct.pack("<I", source_word(s, i, 0)) for i in range(DRAWS))
              for s in range(2)]
    return fnv1a(compute), f"{fnv1a(pixels[0]):08x}{fnv1a(pixels[1]):08x}"


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    variant = artifact.get("variant")
    if variant not in PROFILES or artifact.get("profile") != PROFILES[variant]:
        raise ValueError("unexpected descriptor witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt witness receipt")
    text = log.decode("utf-8", errors="replace")
    mark = "DESCRIPTOR_CAPACITY_WITNESS" if variant == "capacity" else "DESCRIPTOR_DYNAMIC_WITNESS"
    if (mark + "_FAILURE" in text or re.findall(mark + r"_RETIRED resources=(\w+)", text) != ["clean"]):
        raise ValueError("witness failed or did not retire cleanly")
    if variant == "capacity":
        compute, pixels, texels = expected_capacity()
        texel_result = re.findall(mark + r"_TEXEL_RESULT texel_mismatches=(\d+) first_texel=(-?\d+)"
                                  r" guard=([0-9a-f]{8}) digest_texel=([0-9a-f]{8})", text)
        result = re.findall(mark + r"_RESULT compute_mismatches=(\d+) first_compute=(-?\d+)"
                            r" pixel_mismatches=(\d+) first_pixel=(-?\d+) guard=([0-9a-f]{8})"
                            r" digest_compute=([0-9a-f]{8}) digest_pixels=([0-9a-f]{8})", text)
        if (f"{mark}_START images={IMAGES} compute_set={IMAGES} pixel_set={IMAGES}" not in text or
                f"{mark}_UPLOADED images={IMAGES}" not in text or
                result != [("0", "-1", "0", "-1", f"{SENTINEL:08x}", f"{compute:08x}",
                            f"{pixels:08x}")] or
                texel_result != [("0", "-1", f"{SENTINEL:08x}", f"{texels:08x}")]):
            raise ValueError("capacity data, guard or digest mismatch")
    else:
        compute, pixels = expected_dynamic()
        stale = re.findall(mark + r"_STALE_RESUBMIT result=(-?\d+)", text)
        result = re.findall(mark + r"_RESULT compute_mismatches=(\d+) pixel_mismatches=(\d+)"
                            r" guards=(\d+) stale_resubmit=(-?\d+) digest_compute=([0-9a-f]{8})"
                            r" digest_pixels=([0-9a-f]{16})", text)
        if (f"{mark}_START draws={DRAWS} stride={STRIDE}" not in text or len(stale) != 1 or
                int(stale[0]) >= 0 or
                result != [("0", "0", "0", stale[0], f"{compute:08x}", pixels)]):
            raise ValueError("dynamic data, stale refusal or digest mismatch")
    return {"strict_verified": True, "variant": variant, "run_id": receipt["run_id"],
            "log_sha256": receipt["sha256"], "eboot_sha256": artifact["eboot_sha256"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--runs-dir", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()
    if running(args.host) != "none":
        raise RuntimeError("refusing to launch while a title is active")
    artifact = json.loads(args.artifact.read_text())
    if hashlib.sha256((args.dist / "eboot.bin").read_bytes()).hexdigest() != artifact.get("eboot_sha256"):
        raise RuntimeError("artifact identity mismatch")
    known = {path.name for path in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    result, launched = {}, False
    try:
        reply = control("launch", args.host)
        if "Error spawning payload" in reply:
            raise RuntimeError("console rejected witness launch: " + reply.strip())
        launched = True
        log_path = wait_for_log(args.runs_dir, known, args.timeout)
        receipt = json.loads(log_path.with_suffix(".json").read_text())
        result = verify(log_path.read_bytes(), receipt, artifact)
        result["source_log"] = str(log_path)
    finally:
        try:
            lifecycle_ok = close_and_confirm(args.host) if launched else running(args.host) == "none"
        except RuntimeError:
            lifecycle_ok = False
        result["lifecycle_ok"] = lifecycle_ok
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    if not lifecycle_ok:
        raise RuntimeError("witness title did not stop")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
