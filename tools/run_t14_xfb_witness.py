#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK transform feedback witness
(DXVK262-T14)."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t14_xfb_witness import CASES, PROFILE, SWITCH  # noqa: E402
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

START = re.compile(r"T14_XFB_WITNESS_START feature=(\d+) streams_feature=(\d+) geometry=(\d+) "
                   r"streams=(\d+) buffers=(\d+) stride=(\d+) data=(\d+) stream_data=(\d+) "
                   r"queries=(\d+) draw=(\d+)")
CASE = re.compile(r"T14_XFB_WITNESS_CASE name=(\w+) points=(\d+) ok=(\d+) mismatches=(\d+) "
                  r"first_bad=(-?\d+) sentinel_bad=(\d+) before_bad=(\d+) counter0=(\d+) "
                  r"want0=(\d+) stream1_mismatches=(\d+) stream1_sentinel_bad=(\d+) "
                  r"counter1=(\d+) want1=(\d+) w0=([0-9a-f,]+) digest=([0-9a-f]{8}) "
                  r"fence=complete")
RESULT = re.compile(r"T14_XFB_WITNESS_RESULT cases=(\d+) passed=(\d+) submissions=(\d+)")
RETIRED = re.compile(r"T14_XFB_WITNESS_RETIRED resources=(\w+)")
# The oracle the payload applies, restated so a payload that reports ok=1
# with inconsistent numbers is still refused.
EXPECTED = {
    "inactive": (3, 0, 0), "small": (3, 96, 0), "order": (6000, 192000, 0),
    "resume": (3, 224, 0), "overflow": (16, 320, 0), "streams": (4, 64, 64),
}


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("diagnostic_switch") != SWITCH or
            artifact.get("cases") != list(CASES)):
        raise ValueError("unexpected transform feedback witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start, cases = START.findall(text), CASE.findall(text)
    results, retired = RESULT.findall(text), RETIRED.findall(text)
    if (len(start) != 1 or len(results) != 1 or len(retired) != 1 or
            "T14_XFB_WITNESS_FAILURE" in text):
        raise ValueError("missing, repeated or failed witness phase")
    (feature, streams_feature, geometry, streams, buffers, stride, data, stream_data,
     queries, draw) = (int(v) for v in start[0])
    if (feature != 1 or streams_feature != 1 or geometry != 1 or streams != 4 or
            buffers != 4 or stride != 2048 or data != 512 or stream_data != 512 or
            queries != 0 or draw != 0):
        raise ValueError("transform feedback reporting does not match the witness contract")
    if [case[0] for case in cases] != list(CASES):
        raise ValueError("cases missing, repeated or out of order")
    measured = {}
    for (name, points, ok, mismatches, first_bad, sentinel_bad, before_bad, counter0,
         want0, s1_mismatches, s1_sentinel, counter1, want1, _w0, digest) in cases:
        want_points, want_counter0, want_counter1 = EXPECTED[name]
        passed = (int(ok) == 1 and int(points) == want_points and int(mismatches) == 0 and
                  int(first_bad) == -1 and int(sentinel_bad) == 0 and int(before_bad) == 0 and
                  int(counter0) == int(want0) == want_counter0 and
                  int(counter1) == int(want1) == want_counter1 and
                  int(s1_mismatches) == 0 and int(s1_sentinel) == 0)
        measured[name] = {"passed": passed, "mismatches": int(mismatches),
                          "first_bad": int(first_bad), "sentinel_bad": int(sentinel_bad),
                          "counter0": int(counter0), "counter1": int(counter1),
                          "digest": digest}
    count, passed_count, submissions = (int(v) for v in results[0])
    if count != len(CASES) or submissions != len(CASES):
        raise ValueError("witness did not complete every submission")
    if retired[0] != "clean":
        raise ValueError("witness resources were not retired")
    strict = all(case["passed"] for case in measured.values()) and passed_count == len(CASES)
    return {
        "strict_verified": strict,
        "run_id": receipt["run_id"],
        "cases": measured,
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
    return 0 if result.get("strict_verified") else 2


if __name__ == "__main__":
    raise SystemExit(main())
