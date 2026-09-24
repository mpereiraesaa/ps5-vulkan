#!/usr/bin/env python3
"""Run and strictly verify one bounded diagnostic subgroup witness."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402


SOURCE_LANES = [7, 19, 31, 1]


def expected_digest(operation: str = "broadcast") -> int:
    if operation not in ("broadcast", "iadd", "iadd_int8"):
        raise ValueError("unknown subgroup operation")
    digest = 2166136261
    for index in range(128):
        subgroup = index // 32
        if operation == "iadd":
            value = 32 * SOURCE_LANES[subgroup] + 496
        elif operation == "iadd_int8":
            value = (32 * SOURCE_LANES[subgroup] + 496) & 0xff
        else:
            value = ((subgroup // 2) * 1000 + (subgroup % 2) * 100 +
                     SOURCE_LANES[subgroup])
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    return digest


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    operation = artifact.get("operation", "broadcast")
    if (operation not in ("broadcast", "iadd", "iadd_int8") or
            artifact.get("profile") !=
            f"t08-subgroup-{operation}-diagnostic-witness" or
            artifact.get("outputs") != 128 or
            artifact.get("subgroups") != 4 or
            artifact.get("source_lanes") != SOURCE_LANES or
            artifact.get("public_profile") != "vulkan-1.0-subgroup-disabled"):
        raise ValueError("unexpected subgroup witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt witness receipt")
    text = log.decode("utf-8", errors="replace")
    mark = {"broadcast": "T08_SUBGROUP",
            "iadd": "T08_SUBGROUP_IADD",
            "iadd_int8": "T08_SUBGROUP_IADD_INT8"}[operation]
    starts = re.findall(mark + r"_START subgroups=(\d+) outputs=(\d+) "
                        r"ids=([\d,]+) api=([\d.]+)", text)
    results = re.findall(mark + r"_RESULT outputs=(\d+) "
                         r"mismatches=(\d+) guards=(\d+) "
                         r"digest=([0-9a-f]{8}) fence=(\w+)", text)
    retired = re.findall(mark + r"_RETIRED resources=(\w+)", text)
    if (starts != [("4", "128", "7,19,31,1", "1.0")] or
            results != [("128", "0", "0", f"{expected_digest(operation):08x}",
                         "complete")] or
            retired != ["clean"] or
            text.index(mark + "_START") >=
            text.index(mark + "_RESULT") or
            text.index(mark + "_RESULT") >=
            text.index(mark + "_RETIRED") or
            mark + "_FAILURE" in text):
        raise ValueError("subgroup data, guard, fence or cleanup failed")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "outputs": 128,
        "operation": operation,
        "source_lanes": SOURCE_LANES,
        "mismatches": 0,
        "guards": 0,
        "digest": f"{expected_digest(operation):08x}",
        "log_sha256": receipt["sha256"],
        "eboot_sha256": artifact["eboot_sha256"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operation", choices=("broadcast", "iadd", "iadd_int8"),
                        default="broadcast")
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
    if (artifact.get("profile") !=
            f"t08-subgroup-{args.operation}-diagnostic-witness" or
            artifact.get("operation", "broadcast") != args.operation or
            hashlib.sha256(eboot.read_bytes()).hexdigest() !=
            artifact.get("eboot_sha256")):
        raise RuntimeError("artifact identity mismatch")
    known = {path.name for path in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    result = {}
    launched = False
    try:
        launch_reply = control("launch", args.host)
        if "Error spawning payload" in launch_reply:
            raise RuntimeError("console rejected witness launch: " + launch_reply.strip())
        launched = True
        log_path = wait_for_log(args.runs_dir, known, args.timeout)
        receipt = json.loads(log_path.with_suffix(".json").read_text())
        result = verify(log_path.read_bytes(), receipt, artifact)
        result["source_log"] = str(log_path)
    finally:
        try:
            lifecycle_ok = (close_and_confirm(args.host) if launched else
                            running(args.host) == "none")
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
