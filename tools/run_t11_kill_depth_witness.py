#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK pixel-removal witness.

The receipt is valid evidence when the run is complete and its control case is
clean; the verdict then says, per removal form, whether removed pixels stayed
out of the depth and stencil planes. The regression passes only when every
form suppressed its removed pixels and lost no kept one.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

PROFILE = "t11-kill-depth-public-sdk-witness"
EXTENT = 64
CASES = ("control", "kill", "terminate", "demote")
# The pass, then each aspect's readback in its own submission.
SUBMISSIONS_PER_CASE = 3
# (x ^ y) & 1 over a 64x64 target: exactly half the pixels are removed.
REMOVED = EXTENT * EXTENT // 2

CASE = re.compile(
    r"T11_KILL_WITNESS_CASE form=(\w+) removes=(\d) depth_mismatches=(\d+) "
    r"stencil_mismatches=(\d+) removed_written=(\d+) kept_missing=(\d+) "
    r"depth_0_0=([0-9a-f]{8}) depth_1_0=([0-9a-f]{8}) stencil_0_0=([0-9a-f]{2}) "
    r"stencil_1_0=([0-9a-f]{2}) depth_digest=([0-9a-f]{8}) "
    r"stencil_digest=([0-9a-f]{8}) fence=(\w+)")


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("extent") != EXTENT or
            artifact.get("format") != "D32_SFLOAT_S8_UINT" or
            tuple(artifact.get("cases") or ()) != CASES or
            artifact.get("diagnostic_switch") is not None):
        raise ValueError("unexpected pixel-removal witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start = re.findall(r"T11_KILL_WITNESS_START extent=(\d+) cases=(\d+)", text)
    pipelines = re.findall(r"T11_KILL_WITNESS_PIPELINE form=(\w+) created=1", text)
    steps = re.findall(r"T11_KILL_WITNESS_STEP form=(\w+) index=(\d+) fence=complete", text)
    cases = CASE.findall(text)
    result = re.findall(r"T11_KILL_WITNESS_RESULT cases=(\d+) submissions=(\d+) "
                        r"fence=(\w+)", text)
    retired = re.findall(r"T11_KILL_WITNESS_RETIRED resources=(\w+)", text)
    if (start != [(str(EXTENT), str(len(CASES)))] or tuple(pipelines) != CASES or
            tuple(c[0] for c in cases) != CASES or
            [c[1] for c in cases] != ["0", "1", "1", "1"] or
            any(c[12] != "complete" for c in cases) or
            steps != [(form, str(i)) for form in CASES for i in range(SUBMISSIONS_PER_CASE)] or
            result != [(str(len(CASES)), str(len(CASES) * SUBMISSIONS_PER_CASE),
                        "complete")] or
            retired != ["clean"] or "T11_KILL_WITNESS_FAILURE" in text or
            text.index("T11_KILL_WITNESS_START") >= text.index("T11_KILL_WITNESS_RESULT") or
            text.index("T11_KILL_WITNESS_RESULT") >= text.index("T11_KILL_WITNESS_RETIRED")):
        raise ValueError("pixel-removal witness incomplete, unfenced or not retired")
    tallies = {c[0]: {"depth_mismatches": int(c[2]), "stencil_mismatches": int(c[3]),
                      "removed_written": int(c[4]), "kept_missing": int(c[5]),
                      "depth_digest": c[10], "stencil_digest": c[11]} for c in cases}
    # The instrument: the control writes every pixel of both planes.
    if any(tallies["control"][k] for k in ("depth_mismatches", "stencil_mismatches",
                                           "removed_written", "kept_missing")):
        raise ValueError("control case failed: the readback instrument is not valid")
    verdict = {}
    for form in CASES[1:]:
        t = tallies[form]
        if t["kept_missing"]:
            verdict[form] = "kept-pixels-lost"
        elif not (t["depth_mismatches"] or t["stencil_mismatches"]):
            verdict[form] = "removal-suppressed"
        elif t["removed_written"] == REMOVED:
            verdict[form] = "removal-ignored"
        else:
            verdict[form] = "removal-partial"
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "verdict": verdict,
        "regression_ok": all(v == "removal-suppressed" for v in verdict.values()),
        "tallies": tallies,
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
    return 0 if result.get("regression_ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
