#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK VK_KHR_synchronization2 witness."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_sync2_witness import (  # noqa: E402
    GATE_VALUE, INITIAL_VALUE, PHASE_VALUES, PROFILE, SDK_SWITCHES, SEEDS, VALUES,
    expected_digest)
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

START = re.compile(r"DXVK_SYNC2_WITNESS_START sync2_spec=(\d+) timeline_spec=(\d+) "
                   r"commands=(\d+) core_names=(\w+) timestamp_valid_bits=(\d+) "
                   r"initial=(\d+) values=(\d+)")
DEFERRED = re.compile(r"DXVK_SYNC2_WITNESS_DEFERRED submit=(\w+) counter=(\d+) "
                      r"wait_zero=(\w+) wait_20ms=(\w+) untouched=(\w+)")
RESULT = re.compile(r"DXVK_SYNC2_WITNESS_RESULT phase=(\d) buffers=(\d) wait=(\w+) "
                    r"counter=(\d+) values=(\d+) mismatches=(\d+) guard_mismatches=(\d+) "
                    r"digest=([0-9a-f]{8})")
RETIRED = re.compile(r"DXVK_SYNC2_WITNESS_RETIRED resources=(\w+)")
# phase -> (command buffers in the submit2, seed whose data the buffer holds)
PHASES = ((1, 3, SEEDS[0]), (2, 1, SEEDS[1]), (3, 0, SEEDS[1]))


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("values") != VALUES or
            artifact.get("initial_value") != INITIAL_VALUE or
            artifact.get("gate_value") != GATE_VALUE or
            artifact.get("phase_values") != list(PHASE_VALUES) or
            artifact.get("seeds") != list(SEEDS) or
            artifact.get("sdk_switches") != SDK_SWITCHES):
        raise ValueError("unexpected synchronization2 witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start = START.findall(text)
    deferred = DEFERRED.findall(text)
    results = RESULT.findall(text)
    retired = RETIRED.findall(text)
    if (len(start) != 1 or len(deferred) != 1 or len(results) != 3 or
            len(retired) != 1 or "DXVK_SYNC2_WITNESS_FAILURE" in text):
        raise ValueError("missing, repeated or failed witness phase")
    sync2_spec, timeline_spec, commands, core, valid_bits, initial, values = start[0]
    if (int(sync2_spec) < 1 or int(timeline_spec) < 1 or commands != "6" or
            core != "absent" or int(initial) != INITIAL_VALUE or int(values) != VALUES):
        raise ValueError("synchronization2 exposure does not match the witness contract")
    # The gated submit returned, the counter still held phase 1's value, both
    # waits timed out and the buffer still held only phase 1's data.
    if deferred[0] != ("returned", str(PHASE_VALUES[0]), "timeout", "timeout", "yes"):
        raise ValueError("work or payload became visible before the host signal")
    for (phase, buffers, seed), value, got in zip(PHASES, PHASE_VALUES, results):
        expected = (str(phase), str(buffers), "success", str(value), str(VALUES), "0", "0",
                    f"{expected_digest(seed):08x}")
        if got != expected:
            raise ValueError(f"phase {phase} data, counter or guards failed")
    if retired[0] != "clean":
        raise ValueError("witness resources were not retired")
    order = [text.index(marker) for marker in (
        "DXVK_SYNC2_WITNESS_START", "DXVK_SYNC2_WITNESS_RESULT phase=1",
        "DXVK_SYNC2_WITNESS_DEFERRED", "DXVK_SYNC2_WITNESS_RESULT phase=2",
        "DXVK_SYNC2_WITNESS_RESULT phase=3", "DXVK_SYNC2_WITNESS_RETIRED")]
    if order != sorted(order):
        raise ValueError("witness phases are out of order")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "synchronization2_spec_version": int(sync2_spec),
        "timestamp_valid_bits": int(valid_bits),
        "phase_values": list(PHASE_VALUES),
        "digests": [f"{expected_digest(seed):08x}" for seed in SEEDS],
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
