#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK extended-dynamic-state
witness: pipelines with dynamic primitive topology and dynamic vertex input
binding stride draw eight columns (triangle list, strip and fan, the two
triangle adjacency topologies without a geometry stage, and list adjacency
into a geometry stage), and each column must be exactly its own colour on
every pixel."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

PROFILE = "dxvk-eds-public-sdk-witness"
# (VkPrimitiveTopology, stride, vertices, rgba) per column.
EXPECTED = ((3, 16, 6, "ff0000ff"), (4, 16, 4, "00ff00ff"),
            (3, 24, 6, "0000ffff"), (4, 32, 4, "4080c0ff"),
            (5, 16, 4, "ffff00ff"), (8, 16, 12, "ff00ffff"),
            (9, 16, 8, "00ffffff"), (8, 16, 12, "804020ff"))
COLUMN = re.compile(r"DXVK_EDS_WITNESS_COLUMN column=(\d) topology=(\d+) stride=(\d+) "
                    r"vertices=(\d+) rgba=([0-9a-f]{8}) uniform_mismatches=(\d+)")


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("width") != 128 or
            artifact.get("height") != 16 or artifact.get("columns") != 8):
        raise ValueError("unexpected extended dynamic state witness artifact")
    if (receipt.get("protocol") != "ps5log/1" or
            receipt.get("title") != "PPSA99994" or
            receipt.get("app") != "ps5vk" or
            receipt.get("transport") != "tcp" or
            not receipt.get("clean") or not receipt.get("bye") or
            receipt.get("gaps") or
            receipt.get("sha256") != hashlib.sha256(log).hexdigest()):
        raise ValueError("incomplete or corrupt ps5log/1 receipt")
    text = log.decode("utf-8", errors="replace")
    columns = COLUMN.findall(text)
    if (text.count("DXVK_EDS_WITNESS_START width=128 height=16 columns=8") != 1 or
            [c[0] for c in columns] != [str(n) for n in range(8)] or
            text.count("DXVK_EDS_WITNESS_RESULT submissions=2 fence=complete") != 1 or
            text.count("DXVK_EDS_WITNESS_RETIRED resources=clean") != 1 or
            "DXVK_EDS_WITNESS_FAILURE" in text):
        raise ValueError("EDS witness incomplete, unfenced or not retired")
    for column, expected in zip(columns, EXPECTED):
        topology, stride, vertices, rgba = expected
        if (int(column[1]), int(column[2]), int(column[3])) != (topology, stride, vertices):
            raise ValueError(f"column {column[0]} drew an unexpected shape")
        if column[4] != rgba or column[5] != "0":
            raise ValueError(f"column {column[0]} is not exactly its colour")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "columns": [c[4] for c in columns],
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
