#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK helper-invocation witness.

A complete run with a clean control is valid evidence. The helper contract
passes only when every demote spelling leaves its removed pixels unwritten and
every kept pixel exact, which requires the removed lane to keep executing as a
helper for its quad's derivatives. OpKill and OpTerminateInvocation are
reported but not required: the specification leaves derivatives after a
terminated lane undefined.
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

PROFILE = "t11-helper-public-sdk-witness"
EXTENT = 64
CASES = ("control", "kill", "demote", "demote_dxvk", "demote_ext", "terminate")
REQUIRED = ("demote", "demote_dxvk", "demote_ext")
SUBMISSIONS_PER_CASE = 2
# One removed pixel per 2x2 quad.
REMOVED = EXTENT * EXTENT // 4

CASE = re.compile(
    r"T11_HELPER_WITNESS_CASE form=(\w+) removes=(\d) kept_wrong=(\d+) "
    r"removed_written=(\d+) written=(\d+) px_0_0=([0-9a-f]{8}) px_1_0=([0-9a-f]{8}) "
    r"px_0_1=([0-9a-f]{8}) px_1_1=([0-9a-f]{8}) digest=([0-9a-f]{8}) fence=(\w+)")


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("extent") != EXTENT or
            artifact.get("format") != "R8G8B8A8_UNORM" or
            tuple(artifact.get("cases") or ()) != CASES or
            artifact.get("diagnostic_switch") is not None):
        raise ValueError("unexpected helper witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start = re.findall(r"T11_HELPER_WITNESS_START extent=(\d+) cases=(\d+)", text)
    pipelines = re.findall(r"T11_HELPER_WITNESS_PIPELINE form=(\w+) created=1", text)
    steps = re.findall(r"T11_HELPER_WITNESS_STEP form=(\w+) index=(\d+) fence=complete", text)
    cases = CASE.findall(text)
    result = re.findall(r"T11_HELPER_WITNESS_RESULT cases=(\d+) submissions=(\d+) "
                        r"fence=(\w+)", text)
    retired = re.findall(r"T11_HELPER_WITNESS_RETIRED resources=(\w+)", text)
    if (start != [(str(EXTENT), str(len(CASES)))] or tuple(pipelines) != CASES or
            tuple(c[0] for c in cases) != CASES or
            [c[1] for c in cases] != ["0"] + ["1"] * (len(CASES) - 1) or
            any(c[10] != "complete" for c in cases) or
            steps != [(form, str(i)) for form in CASES for i in range(SUBMISSIONS_PER_CASE)] or
            result != [(str(len(CASES)), str(len(CASES) * SUBMISSIONS_PER_CASE),
                        "complete")] or
            retired != ["clean"] or "T11_HELPER_WITNESS_FAILURE" in text or
            text.index("T11_HELPER_WITNESS_START") >= text.index("T11_HELPER_WITNESS_RESULT") or
            text.index("T11_HELPER_WITNESS_RESULT") >= text.index("T11_HELPER_WITNESS_RETIRED")):
        raise ValueError("helper witness incomplete, unfenced or not retired")
    tallies = {c[0]: {"kept_wrong": int(c[2]), "removed_written": int(c[3]),
                      "written": int(c[4]), "digest": c[9],
                      "quad_0_0": [c[5], c[6], c[7], c[8]]} for c in cases}
    control_tally = tallies["control"]
    if control_tally["kept_wrong"] or control_tally["written"] != EXTENT * EXTENT:
        raise ValueError("control case failed: the instrument is not valid")
    verdict = {}
    for form in CASES[1:]:
        t = tallies[form]
        if t["removed_written"]:
            verdict[form] = "removal-ignored"
        elif t["kept_wrong"]:
            verdict[form] = "helper-derivatives-wrong"
        else:
            verdict[form] = "helpers-live"
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "verdict": verdict,
        "helper_ok": all(verdict[form] == "helpers-live" for form in REQUIRED),
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
    return 0 if result.get("helper_ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
