#!/usr/bin/env python3
"""Run and strictly verify one bounded public-SDK robustness2 vertex-input witness.

Column 0 draws from a VK_NULL_HANDLE vertex buffer and column 1 entirely past
a three-vertex buffer; both must read the zero attribute, (0,0,0,0) or
(0,0,0,1). Column 2 draws the same buffer in bounds and must show the stored
value (0.25, 0.5, 0.75, 1.0) as RGBA8, within one step per channel."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_consumer import close_and_confirm, control, running, wait_for_log  # noqa: E402

PROFILE = "t13-vertex-robustness-public-sdk-witness"
STORED = (64, 128, 191, 255)
COLUMN = re.compile(r"T13_VERTEX_WITNESS_COLUMN column=(\d) rgba=([0-9a-f]{8}) "
                    r"uniform_mismatches=(\d+)")


def rgba(word: str) -> tuple[int, ...]:
    return tuple(int(word[i:i + 2], 16) for i in range(0, 8, 2))


def verify(log: bytes, receipt: dict, artifact: dict) -> dict:
    if (artifact.get("profile") != PROFILE or artifact.get("width") != 48 or
            artifact.get("height") != 16 or artifact.get("columns") != 3):
        raise ValueError("unexpected vertex robustness witness artifact")
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
    if (text.count("T13_VERTEX_WITNESS_START width=48 height=16 columns=3") != 1 or
            [c[0] for c in columns] != ["0", "1", "2"] or
            text.count("T13_VERTEX_WITNESS_RESULT submissions=2 fence=complete") != 1 or
            text.count("T13_VERTEX_WITNESS_RETIRED resources=clean") != 1 or
            "T13_VERTEX_WITNESS_FAILURE" in text):
        raise ValueError("vertex witness incomplete, unfenced or not retired")
    if any(c[2] != "0" for c in columns):
        raise ValueError("a column is not one flat colour")
    zero_alpha = []
    for column in columns[:2]:
        value = rgba(column[1])
        if value[:3] != (0, 0, 0) or value[3] not in (0, 255):
            raise ValueError(f"column {column[0]} did not read the zero attribute")
        zero_alpha.append(value[3])
    control_value = rgba(columns[2][1])
    if any(abs(a - b) > 1 for a, b in zip(control_value, STORED)):
        raise ValueError("the in-bounds control did not read the stored value")
    return {
        "strict_verified": True,
        "run_id": receipt["run_id"],
        "null_buffer_rgba": columns[0][1],
        "past_buffer_rgba": columns[1][1],
        "control_rgba": columns[2][1],
        "zero_alpha": zero_alpha,
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
