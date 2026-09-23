#!/usr/bin/env python3
"""Run and verify one bounded public-SDK memory-model witness."""

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
    for index in range(128):
        value = ((index * 17) ^ 0x59a31c4d) ^ 0x9e3779b9
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    return digest


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    scope = artifact.get("scope")
    if scope not in ("queue-family", "device"):
        raise ValueError("unknown witness scope")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start = re.findall(r"T08_MEMORY_MODEL_WITNESS_START scope=(\S+) values=(\d+) "
                       r"guards=(\d+) negative_gate=(\w+)", text)
    result = re.findall(r"T08_MEMORY_MODEL_WITNESS_RESULT scope=(\S+) values=(\d+) "
                        r"mismatches=(\d+) guard_mismatches=(\d+) "
                        r"digest=([0-9a-f]{8}) fence=(\w+)", text)
    retired = re.findall(r"T08_MEMORY_MODEL_WITNESS_RETIRED scope=(\S+) "
                         r"resources=(\w+)", text)
    litmus = re.findall(r"T08_MEMORY_MODEL_LITMUS scope=(\S+) pairs=(\d+) "
                        r"observed=(\d+) skipped=(\d+) failures=(\d+) "
                        r"guard_mismatches=(\d+) fence=(\w+)", text)
    if (len(start) != 1 or len(result) != 1 or len(litmus) != 1 or
            len(retired) != 1 or
            start[0] != (scope, "128", "16", "pass") or
            result[0] != (scope, "128", "0", "0", f"{expected_digest():08x}",
                          "complete") or
            litmus[0][0] != scope or litmus[0][1] != "1024" or
            int(litmus[0][2]) < 32 or
            int(litmus[0][2]) + int(litmus[0][3]) != 1024 or
            litmus[0][4:] != ("0", "0", "complete") or
            retired[0] != (scope, "clean") or
            text.index("T08_MEMORY_MODEL_WITNESS_START") >=
            text.index("T08_MEMORY_MODEL_WITNESS_RESULT") or
            text.index("T08_MEMORY_MODEL_WITNESS_RESULT") >=
            text.index("T08_MEMORY_MODEL_LITMUS") or
            text.index("T08_MEMORY_MODEL_LITMUS") >=
            text.index("T08_MEMORY_MODEL_WITNESS_RETIRED") or
            "T08_MEMORY_MODEL_CHECK" in text or
            "T08_MEMORY_MODEL_REQUIRE" in text):
        raise ValueError("witness data, guards, fence or cleanup failed")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "scope": scope,
        "values": 128,
        "litmus_pairs": 1024,
        "litmus_observed": int(litmus[0][2]),
        "litmus_skipped": int(litmus[0][3]),
        "guard_mismatches": 0,
        "mismatches": 0,
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
    if (artifact.get("profile") != "t08-memory-model-public-sdk-witness" or
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
        lifecycle_ok = close_and_confirm(args.host) if launched else running(args.host) == "none"
        result["lifecycle_ok"] = lifecycle_ok
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    if not lifecycle_ok:
        raise RuntimeError("witness title did not stop")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
