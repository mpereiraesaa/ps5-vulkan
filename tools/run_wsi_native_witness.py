#!/usr/bin/env python3
"""Run and verify three bounded native WSI frames and clean title shutdown."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_wsi_native_witness import PROFILE  # noqa: E402
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

START = re.compile(r"WSI_WITNESS_START display=1920x1080 refresh=60000 "
                   r"image_count=2 usage=(\d+)")
ADAPTER = re.compile(r"WSI_WITNESS_ADAPTER surface=created")
FRAME = re.compile(r"WSI_WITNESS_FRAME frame=(\d+) slot=([01]) color=([012]) "
                   r"fence=complete present=complete")
RETIRED = re.compile(r"WSI_WITNESS_RETIRED frames=3 resources=clean")
REGISTER = re.compile(r"PS5VK_VIDEO_REGISTER handle=(-?\d+) buffers=2 "
                      r"image_bytes=(\d+) format_word=([0-9a-fA-F]{16})")
PRESENTED = re.compile(r"PS5VK_VIDEO_PRESENTED token=(\d+) fence=0 "
                       r"matching_event=1 hold_seconds=0")
CLOSED = re.compile(r"PS5VK_VIDEO_CLOSED token=(\d+) deferred=(\d+)")


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if artifact.get("profile") != PROFILE:
        raise ValueError("wrong witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt witness log")
    content = log.decode("utf-8", errors="replace")
    start = START.findall(content)
    adapter = ADAPTER.findall(content)
    frames = FRAME.findall(content)
    retired = RETIRED.findall(content)
    registered = REGISTER.findall(content)
    presented = PRESENTED.findall(content)
    closed = CLOSED.findall(content)
    if (len(start) != 1 or len(adapter) != 1 or len(frames) != 3 or
            len(retired) != 1 or
            len(registered) != 1 or len(presented) != 3 or len(closed) != 1 or
            [int(token) for token in presented] != [1, 2, 3] or
            int(registered[0][0]) < 0 or int(registered[0][1]) < 8912896 or
            closed[0][0] != "3" or closed[0][1] not in ("0", "1") or
            "PS5VK_VIDEO_RETAIN" in content or
            "PS5VK_VIDEO_SETUP_FAILED" in content or
            "WSI_WITNESS_FAILURE" in content):
        raise ValueError("missing, repeated or failed witness phase")
    if start[0] != str(0x10 | 0x2):
        raise ValueError("unexpected swapchain image usage")
    if [tuple(int(x) for x in row) for row in frames] != [
            (0, 0, 0), (1, 1, 1), (2, 0, 2)]:
        raise ValueError("wrong frame order, color or reuse")
    markers = ["WSI_WITNESS_ADAPTER", "WSI_WITNESS_START",
               "PS5VK_VIDEO_REGISTER",
               "PS5VK_VIDEO_PRESENTED token=1", "WSI_WITNESS_FRAME frame=0",
               "PS5VK_VIDEO_PRESENTED token=2", "WSI_WITNESS_FRAME frame=1",
               "PS5VK_VIDEO_PRESENTED token=3", "WSI_WITNESS_FRAME frame=2",
               "PS5VK_VIDEO_CLOSED", "WSI_WITNESS_RETIRED"]
    if [content.index(marker) for marker in markers] != sorted(
            content.index(marker) for marker in markers):
        raise ValueError("witness events out of order")
    # deferred=1 means unregister reported RESOURCE_BUSY and the successful
    # VideoOut close retired that registration. CLOSED is emitted only after
    # close succeeds; clean BYE and absence of RETAIN remain required above.
    return {"strict_verified": True, "run_id": receipt["run_id"],
            "frames": 3, "slots": [0, 1, 0],
            "unregister_deferred_to_close": closed[0][1] == "1",
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
            hashlib.sha256(eboot.read_bytes()).hexdigest() !=
            artifact.get("eboot_sha256")):
        raise RuntimeError("artifact identity mismatch")
    known = {path.name for path in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    result = {}
    launched = False
    try:
        control("launch", args.host)
        launched = True
        log_path = wait_for_log(args.runs_dir, known, args.timeout)
        receipt = json.loads(log_path.with_suffix(".json").read_text())
        result = verify(log_path.read_bytes(), receipt, artifact)
        result["source_log"] = str(log_path)
    finally:
        result["lifecycle_ok"] = (close_and_confirm(args.host) if launched else
                                  running(args.host) == "none")
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    if not result["lifecycle_ok"]:
        raise RuntimeError("witness title did not stop")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
