#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK DXVK routes witness
(memory requirements2, dedicated allocation, bind2, descriptor update template)."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_routes_witness import (  # noqa: E402
    PROFILE, ROUTE_SWITCHES, SEEDS, TARGETS, VALUES)
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

START = re.compile(r"DXVK_ROUTES_WITNESS_START memreq2=(\d+) dedicated=(\d+) bind2=(\d+) "
                   r"template=(\d+) commands=(\d+) values=(\d+) targets=(\d+)")
REQUIREMENTS = re.compile(
    r"DXVK_ROUTES_WITNESS_REQUIREMENTS buffer_size=(\d+) buffer_alignment=(\d+) "
    r"buffer_agrees=(\d) buffer_prefers=(\d) buffer_requires=(\d) image_size=(\d+) "
    r"image_alignment=(\d+) image_agrees=(\d) image_prefers=(\d) image_requires=(\d)")
BIND = re.compile(r"DXVK_ROUTES_WITNESS_BIND dedicated_buffer=1 dedicated_image=1 "
                  r"shared_binds=2 shared_offset=(\d+)")
RESULT = re.compile(r"DXVK_ROUTES_WITNESS_RESULT dispatch=(\d+) target=(\d+) "
                    r"mismatches=(\d+) guard_mismatches=(\d+) fence=(\w+)")
RETIRED = re.compile(r"DXVK_ROUTES_WITNESS_RETIRED resources=(\w+) dispatches=(\d+)")


def expected_results() -> list[tuple[str, ...]]:
    """Each dispatch reports its own target; the last also reports the rest."""
    rows = []
    for dispatch in range(TARGETS):
        targets = range(TARGETS) if dispatch == TARGETS - 1 else (dispatch,)
        for target in targets:
            rows.append((str(dispatch), str(target), "0", "0", "complete"))
    return rows


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("values") != VALUES or
            artifact.get("targets") != TARGETS or artifact.get("seeds") != list(SEEDS) or
            artifact.get("sdk_switches") != ROUTE_SWITCHES):
        raise ValueError("unexpected DXVK routes witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    start, requirements = START.findall(text), REQUIREMENTS.findall(text)
    bind, results, retired = BIND.findall(text), RESULT.findall(text), RETIRED.findall(text)
    if (len(start) != 1 or len(requirements) != 1 or len(bind) != 1 or len(retired) != 1 or
            "DXVK_ROUTES_WITNESS_FAILURE" in text):
        raise ValueError("missing, repeated or failed witness step")
    if (any(int(spec) < 1 for spec in start[0][:4]) or start[0][4:] !=
            ("7", str(VALUES), str(TARGETS))):
        raise ValueError("route exposure does not match the witness contract")
    req = requirements[0]
    if req[2] != "1" or req[7] != "1" or int(req[0]) == 0 or int(req[5]) == 0:
        raise ValueError("*2 requirements disagree with the Vulkan 1.0 queries")
    size, alignment = int(req[0]), int(req[1])
    stride = (size + alignment - 1) // alignment * alignment
    if int(bind[0]) != stride or stride == 0:
        raise ValueError("shared allocation offset is not the aligned requirement size")
    if results != expected_results():
        raise ValueError("a template-updated dispatch wrote the wrong words")
    if retired[0] != ("clean", str(TARGETS)):
        raise ValueError("witness resources were not retired")
    order = [text.index(marker) for marker in (
        "DXVK_ROUTES_WITNESS_START", "DXVK_ROUTES_WITNESS_REQUIREMENTS",
        "DXVK_ROUTES_WITNESS_BIND", "DXVK_ROUTES_WITNESS_RESULT",
        "DXVK_ROUTES_WITNESS_RETIRED")]
    if order != sorted(order):
        raise ValueError("witness steps are out of order")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "spec_versions": [int(spec) for spec in start[0][:4]],
        "buffer_requirements": {"size": size, "alignment": alignment,
                                "prefers_dedicated": req[3] == "1",
                                "requires_dedicated": req[4] == "1"},
        "image_requirements": {"size": int(req[5]), "alignment": int(req[6]),
                               "prefers_dedicated": req[8] == "1",
                               "requires_dedicated": req[9] == "1"},
        "shared_offset": stride,
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
