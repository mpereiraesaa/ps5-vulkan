#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK separate-sampler witness."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_separate_sampler_witness import (  # noqa: E402
    HEIGHT, PROFILE, SWITCH, WIDTH, data_header, expected_pixels, expected_rgba32f,
    expected_rgba8, expected_u32)
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

START = re.compile(r"DXVK_SEPARATE_SAMPLER_WITNESS_START width=(\d+) height=(\d+) "
                   r"r32ui=([0-9a-f]{8}) rgba8=([0-9a-f]{8}) rgba32f=([0-9a-f]{8}) "
                   r"r32i=([0-9a-f]{8})")
REFUSAL = re.compile(r"DXVK_SEPARATE_SAMPLER_WITNESS_REFUSAL view=r32i_storage_texel "
                     r"result=(-?\d+)")
RESULT = re.compile(r"DXVK_SEPARATE_SAMPLER_WITNESS_RESULT u32_mismatches=(\d+) "
                    r"rgba8_mismatches=(\d+) rgba32f_mismatches=(\d+) "
                    r"pixel_mismatches=(\d+) digest_u32=([0-9a-f]{8}) "
                    r"digest_rgba8=([0-9a-f]{8}) digest_rgba32f=([0-9a-f]{8}) "
                    r"digest_pixels=([0-9a-f]{8})")
RETIRED = re.compile(r"DXVK_SEPARATE_SAMPLER_WITNESS_RETIRED resources=(\w+)")
UNIFORM_TEXEL = 0x8
STORAGE_TEXEL = 0x10


def fnv1a(payload: bytes) -> int:
    digest = 2166136261
    for value in payload:
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    return digest


def expected_digests() -> tuple[int, int, int, int]:
    u32 = struct.pack(f"<{WIDTH}I", *expected_u32())
    return (fnv1a(u32), fnv1a(expected_rgba8()), fnv1a(expected_rgba32f()),
            fnv1a(expected_pixels()))


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("width") != WIDTH or
            artifact.get("height") != HEIGHT or
            artifact.get("diagnostic_switch") != SWITCH or
            artifact.get("data_sha256") != hashlib.sha256(data_header().encode()).hexdigest()):
        raise ValueError("unexpected separate-sampler witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start, refusal = START.findall(text), REFUSAL.findall(text)
    results, retired = RESULT.findall(text), RETIRED.findall(text)
    if (len(start) != 1 or len(refusal) != 1 or len(results) != 1 or len(retired) != 1 or
            "DXVK_SEPARATE_SAMPLER_WITNESS_FAILURE" in text):
        raise ValueError("missing, repeated or failed witness phase")
    width, height, r32ui, rgba8, rgba32f, r32i = start[0]
    if (int(width) != WIDTH or int(height) != HEIGHT or
            not all(int(value, 16) & STORAGE_TEXEL for value in (r32ui, rgba8, rgba32f)) or
            int(r32i, 16) & STORAGE_TEXEL or not int(r32ui, 16) & UNIFORM_TEXEL):
        raise ValueError("storage-texel reporting does not match the witness contract")
    if int(refusal[0]) == 0:
        raise ValueError("an R32_SINT storage-texel view was created")
    counts, digests = results[0][:4], tuple(int(value, 16) for value in results[0][4:])
    if any(int(value) for value in counts) or digests != expected_digests():
        raise ValueError("separate-sampler results failed their oracle")
    if retired[0] != "clean":
        raise ValueError("witness resources were not retired")
    order = [text.index(marker) for marker in (
        "DXVK_SEPARATE_SAMPLER_WITNESS_START", "DXVK_SEPARATE_SAMPLER_WITNESS_REFUSAL",
        "DXVK_SEPARATE_SAMPLER_WITNESS_RESULT", "DXVK_SEPARATE_SAMPLER_WITNESS_RETIRED")]
    if order != sorted(order):
        raise ValueError("witness phases are out of order")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "buffer_features": {"r32ui": r32ui, "rgba8": rgba8, "rgba32f": rgba32f,
                            "r32i": r32i},
        "digests": {"u32": results[0][4], "rgba8": results[0][5],
                    "rgba32f": results[0][6], "pixels": results[0][7]},
        "log_sha256": receipt["sha256"],
        "eboot_sha256": artifact["eboot_sha256"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--runs-dir", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()
    if running(args.host) != "none":
        raise RuntimeError("refusing to launch while a title is active")
    artifact = json.loads(args.artifact.read_text())
    eboot = args.dist / "eboot.bin"
    if (artifact.get("profile") != PROFILE or
            hashlib.sha256(eboot.read_bytes()).hexdigest() != artifact.get("eboot_sha256")):
        raise RuntimeError("artifact identity mismatch")
    known = {path.name for path in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    result = {}
    lifecycle_ok = False
    launched = False
    try:
        control("launch", args.host)
        launched = True
        log_path = wait_for_log(args.runs_dir, known, args.timeout)
        receipt = json.loads(log_path.with_suffix(".json").read_text())
        result = verify(log_path.read_bytes(), receipt, artifact)
        result["source_log"] = str(log_path)
    finally:
        lifecycle_ok = (close_and_confirm(args.host) if launched else
                        running(args.host) == "none")
        result["lifecycle_ok"] = lifecycle_ok
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    if not lifecycle_ok:
        raise RuntimeError("witness title did not stop")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
