#!/usr/bin/env python3
"""Run and strictly verify the full-surface BGRA8 readback witness."""

import argparse
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_bgra_full_readback import FRAMES, HEIGHT, PROFILE, WIDTH  # noqa: E402
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

START = re.compile(r"DXVK_BGRA_FULL_READBACK_START width=(\d+) height=(\d+) bytes=(\d+) "
                   r"usage=(\d+) frames=(\d+)")
FRAME = re.compile(r"DXVK_BGRA_FULL_READBACK_FRAME frame=(\d+) mismatches=(\d+) "
                   r"guard=(\d+) digest=([0-9a-f]{8}) fence=complete")
RESULT = re.compile(r"DXVK_BGRA_FULL_READBACK_RESULT frames=(\d+) passed=(\d+)")
RETIRED = re.compile(r"DXVK_BGRA_FULL_READBACK_RETIRED resources=(\w+)")


@lru_cache(maxsize=FRAMES)
def expected_digest(frame: int) -> str:
    pixel = bytes((0, 255 if frame else 0, 0 if frame else 255, 255))
    value = 2166136261
    for byte in pixel * (WIDTH * HEIGHT):
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return f"{value:08x}"


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("width") != WIDTH or
            artifact.get("height") != HEIGHT or artifact.get("frames") != FRAMES or
            artifact.get("bytes_per_frame") != WIDTH * HEIGHT * 4):
        raise ValueError("unexpected BGRA readback artifact")
    if (receipt.get("protocol") != "ps5log/1" or receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    content = log.decode("utf-8", errors="replace")
    starts, frames = START.findall(content), FRAME.findall(content)
    results, retired = RESULT.findall(content), RETIRED.findall(content)
    if (len(starts) != 1 or starts[0] != (str(WIDTH), str(HEIGHT),
            str(WIDTH * HEIGHT * 4), "23", str(FRAMES)) or
            [int(frame[0]) for frame in frames] != list(range(FRAMES)) or
            results != [(str(FRAMES), str(FRAMES))] or retired != ["clean"] or
            "DXVK_BGRA_FULL_READBACK_FAILURE" in content):
        raise ValueError("missing, repeated or failed readback phase")
    checked = {}
    for index, mismatches, guard, digest in frames:
        frame = int(index)
        checked[index] = {"mismatches": int(mismatches), "guard": int(guard),
                          "digest": digest,
                          "passed": int(mismatches) == 0 and int(guard) == 0 and
                                    digest == expected_digest(frame)}
    return {"strict_verified": all(item["passed"] for item in checked.values()),
            "run_id": receipt["run_id"], "frames": checked,
            "log_sha256": receipt["sha256"],
            "eboot_sha256": artifact["eboot_sha256"]}


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
        result["source_log"] = str(log_path)
        receipt = json.loads(log_path.with_suffix(".json").read_text())
        result.update(verify(log_path.read_bytes(), receipt, artifact))
    finally:
        lifecycle_ok = (close_and_confirm(args.host) if launched else
                        running(args.host) == "none")
        result["lifecycle_ok"] = lifecycle_ok
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    if not lifecycle_ok:
        raise RuntimeError("witness title did not stop")
    print(json.dumps(result, indent=2))
    return 0 if result.get("strict_verified") else 2


if __name__ == "__main__":
    raise SystemExit(main())
