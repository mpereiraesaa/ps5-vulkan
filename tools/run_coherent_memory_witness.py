#!/usr/bin/env python3
"""Run and strictly verify one bounded HOST_COHERENT memory witness."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_coherent_memory_witness import PROFILE, SWITCHES, WORDS  # noqa: E402
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

START = re.compile(r"COHERENT_WITNESS_START types=(\d+) type0=([0-9a-f]+) "
                   r"type1=([0-9a-f]+) coherent_type=(-?\d+) words=(\d+)")
C1 = re.compile(r"COHERENT_WITNESS_RESULT case=C1 mismatches=(\d+)")
C2 = re.compile(r"COHERENT_WITNESS_RESULT case=C2 mismatches=(\d+) stale=(\d+)")
C3 = re.compile(r"COHERENT_WITNESS_RESULT case=C3 mismatches=(\d+) stale=(\d+) host_half=(\d+)")
CONTROL = re.compile(r"COHERENT_WITNESS_CONTROL maintained_mismatches=(\d+) "
                     r"unmaintained_mismatches=(\d+) stale_host_read=(\d+) verdict=([a-z-]+)")
RETIRED = re.compile(r"COHERENT_WITNESS_RETIRED resources=(\w+)")
# DEVICE_LOCAL|HOST_VISIBLE and the same plus HOST_COHERENT.
TYPE0, TYPE1 = 0x3, 0x7


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("words") != WORDS or
            artifact.get("sdk_switches") != SWITCHES):
        raise ValueError("unexpected coherent memory witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    found = {name: pattern.findall(text) for name, pattern in (
        ("start", START), ("c1", C1), ("c2", C2), ("c3", C3),
        ("control", CONTROL), ("retired", RETIRED))}
    if (any(len(rows) != 1 for rows in found.values()) or
            "COHERENT_WITNESS_FAILURE" in text):
        raise ValueError("missing, repeated or failed witness case")
    types, type0, type1, coherent_type, words = found["start"][0]
    if (int(types) != 2 or int(type0, 16) != TYPE0 or int(type1, 16) != TYPE1 or
            int(coherent_type) != 1 or int(words) != WORDS):
        raise ValueError("diagnostic memory profile does not match the witness contract")
    if found["c1"][0] != "0" or found["c2"][0][0] != "0" or found["c3"][0][0] != "0":
        raise ValueError("coherent memory lost data without flush or invalidate")
    maintained, unmaintained, stale, verdict = found["control"][0]
    if maintained != "0":
        raise ValueError("control failed even with explicit flush and invalidate")
    expected_verdict = "stale-observed" if unmaintained != "0" else "no-stale-observed"
    if verdict != expected_verdict:
        raise ValueError("control verdict does not match its counts")
    if found["retired"][0] != "clean":
        raise ValueError("witness resources were not retired")
    order = [text.index(marker) for marker in (
        "COHERENT_WITNESS_START", "case=C1", "case=C2", "case=C3",
        "COHERENT_WITNESS_CONTROL", "COHERENT_WITNESS_RETIRED")]
    if order != sorted(order):
        raise ValueError("witness cases are out of order")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        # Whether the missing cache operations were observable on the
        # non-coherent type: "no-stale-observed" means this run cannot show
        # that the driver's maintenance was necessary, only that it is correct.
        "control_verdict": verdict,
        "control_unmaintained_mismatches": int(unmaintained),
        "control_stale_host_reads": int(stale),
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
