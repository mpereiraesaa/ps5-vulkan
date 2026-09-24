#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK depth/stencil witness."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

EXTENT = 64
# DEPTH_STENCIL_ATTACHMENT | TRANSFER_SRC, and nothing claimed for D24S8.
D32S8_FEATURES = 0x00000200 | 0x00004000
STEPS = 6


def fnv(data: bytes) -> int:
    digest = 2166136261
    for byte in data:
        digest = ((digest ^ byte) * 16777619) & 0xffffffff
    return digest


def expected_depth(x: int, y: int) -> float:
    return (x + 0.5 + 64.0 * (y + 0.5)) / 8192.0


def expected_stencil_plane() -> bytes:
    """0xa5 ^ g(x, y): the cleared value inverted bit by bit."""
    return bytes(0xa5 ^ ((x ^ y) & 63) ^ (((y >> 4) & 3) << 6)
                 for y in range(EXTENT) for x in range(EXTENT))


def expected_stencil_digest() -> str:
    return f"{fnv(expected_stencil_plane()):08x}"


def depth_sample_ok(word: str, x: int, y: int) -> bool:
    value = struct.unpack("<f", int(word, 16).to_bytes(4, "little"))[0]
    return abs(value - expected_depth(x, y)) < 1.0 / 65536.0


SAMPLE_POINTS = ((0, 0), (EXTENT - 1, 0), (0, EXTENT - 1), (EXTENT - 1, EXTENT - 1))


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != "t09-depth-stencil-public-sdk-witness" or
            artifact.get("extent") != EXTENT or
            artifact.get("format") != "D32_SFLOAT_S8_UINT"):
        raise ValueError("unexpected depth/stencil witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start = re.findall(r"T09_DS_WITNESS_START extent=(\d+) d32s8=([0-9a-f]{8}) "
                       r"d24s8=([0-9a-f]{8})", text)
    steps = re.findall(r"T09_DS_WITNESS_STEP index=(\d+) fence=complete", text)
    samples = re.findall(r"T09_DS_WITNESS_SAMPLES depth_0_0=([0-9a-f]{8}) "
                         r"depth_63_0=([0-9a-f]{8}) depth_0_63=([0-9a-f]{8}) "
                         r"depth_63_63=([0-9a-f]{8}) stencil_0_0=([0-9a-f]{2}) "
                         r"stencil_63_0=([0-9a-f]{2}) stencil_0_63=([0-9a-f]{2}) "
                         r"stencil_63_63=([0-9a-f]{2})", text)
    result = re.findall(r"T09_DS_WITNESS_RESULT extent=(\d+) depth_mismatches=(\d+) "
                        r"stencil_mismatches=(\d+) depth_after_stencil_mismatches=(\d+) "
                        r"stencil_after_depth_mismatches=(\d+) depth_digest=([0-9a-f]{8}) "
                        r"stencil_digest=([0-9a-f]{8}) submissions=(\d+) fence=(\w+)", text)
    retired = re.findall(r"T09_DS_WITNESS_RETIRED resources=(\w+)", text)
    stencil_digest = expected_stencil_digest()
    plane = expected_stencil_plane()
    if (len(start) != 1 or len(result) != 1 or len(samples) != 1 or len(retired) != 1 or
            int(start[0][0]) != EXTENT or
            int(start[0][1], 16) & D32S8_FEATURES != D32S8_FEATURES or
            int(start[0][2], 16) != 0 or
            steps != [str(i) for i in range(STEPS)] or
            not all(depth_sample_ok(samples[0][i], *SAMPLE_POINTS[i]) for i in range(4)) or
            [int(samples[0][4 + i], 16) for i in range(4)] !=
            [plane[y * EXTENT + x] for x, y in SAMPLE_POINTS] or
            result[0][:5] != (str(EXTENT), "0", "0", "0", "0") or
            result[0][6:] != (stencil_digest, str(STEPS), "complete") or
            retired[0] != "clean" or
            text.index("T09_DS_WITNESS_START") >= text.index("T09_DS_WITNESS_RESULT") or
            text.index("T09_DS_WITNESS_RESULT") >= text.index("T09_DS_WITNESS_RETIRED") or
            "T09_DS_WITNESS_FAILURE" in text):
        raise ValueError("depth/stencil data, transitions, fences or cleanup failed")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "extent": EXTENT,
        "d32s8_features": start[0][1],
        "d24s8_features": start[0][2],
        "depth_digest": result[0][5],
        "stencil_digest": stencil_digest,
        "mismatches": [0, 0, 0, 0],
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
    if (artifact.get("profile") != "t09-depth-stencil-public-sdk-witness" or
            hashlib.sha256(eboot.read_bytes()).hexdigest() !=
            artifact.get("eboot_sha256")):
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
