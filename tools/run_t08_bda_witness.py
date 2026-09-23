#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK buffer-address witness."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402


def expected_digest() -> int:
    digest = 2166136261
    for index in range(64):
        left = (index * 17) ^ 0x59a31c4d
        right = (index * 29) ^ 0x8124e763
        value = ((left ^ 0xa5a55a5a) + right * 3) & 0xffffffff
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    return digest


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != "t08-bda-public-sdk-witness" or
            artifact.get("values") != 64 or
            artifact.get("guard_words_per_buffer") != 8 or
            artifact.get("source_offset_words") != 4):
        raise ValueError("unexpected BDA witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start = re.findall(r"T08_BDA_WITNESS_START values=(\d+) guards=(\d+) "
                       r"source_offset=(\d+) query=(\w+)", text)
    result = re.findall(r"T08_BDA_WITNESS_RESULT values=(\d+) "
                        r"source_offset=(\d+) "
                        r"bind_offsets=(\d+),(\d+),(\d+) "
                        r"mismatches=(\d+) guard_mismatches=(\d+) "
                        r"digest=([0-9a-f]{8}) fence=(\w+)", text)
    retired = re.findall(r"T08_BDA_WITNESS_RETIRED resources=(\w+) "
                         r"gpu_addresses=(\w+)", text)
    if (len(start) != 1 or len(result) != 1 or len(retired) != 1 or
            start[0] != ("64", "8", "4", "khr") or
            result[0][:2] != ("64", "4") or
            result[0][5:] != ("0", "0", f"{expected_digest():08x}",
                              "complete") or
            any(int(offset) < 16 or int(offset) % 16
                for offset in result[0][2:5]) or
            retired[0] != ("clean", "distinct") or
            text.index("T08_BDA_WITNESS_START") >=
            text.index("T08_BDA_WITNESS_RESULT") or
            text.index("T08_BDA_WITNESS_RESULT") >=
            text.index("T08_BDA_WITNESS_RETIRED") or
            "T08_BDA_WITNESS_FAILURE" in text):
        raise ValueError("BDA data, guard, fence or cleanup failed")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "values": 64,
        "source_offset_words": 4,
        "bind_offsets": [int(offset) for offset in result[0][2:5]],
        "mismatches": 0,
        "guard_mismatches": 0,
        "digest": f"{expected_digest():08x}",
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
    if (artifact.get("profile") != "t08-bda-public-sdk-witness" or
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
