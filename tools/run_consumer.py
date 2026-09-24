#!/usr/bin/env python3
"""Run, strictly verify, and cleanly close the public SDK consumer on PS5."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
CONTROL = ROOT / "tools/control.py"
sys.path.insert(0, str(ROOT / "tools"))
from verify_consumer_resource_abi import validate  # noqa: E402
from verify_cube_array_witness import validate as validate_cube_array  # noqa: E402


def control(action: str, host: str) -> str:
    result = subprocess.run(
        [sys.executable, str(CONTROL), action, "--host", host],
        capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError((result.stdout or "") + (result.stderr or ""))
    return (result.stdout or "") + (result.stderr or "")


def running(host: str) -> str:
    match = re.search(r"running=(\S+)", control("status", host))
    if not match:
        raise RuntimeError("could not parse title status")
    return match.group(1)


def wait_for_log(runs_dir: Path, known: set[str], timeout: float,
                 app: str = "ps5vk") -> Path:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        candidates = sorted(
            path for path in runs_dir.glob(f"*_PPSA99994_{app}_*.log")
            if path.name not in known)
        if candidates:
            candidate = candidates[-1]
            if "BYE seq=" in candidate.read_text(encoding="utf-8", errors="replace"):
                receipt = candidate.with_suffix(".json")
                for _ in range(30):
                    if receipt.is_file():
                        return candidate
                    time.sleep(0.1)
        time.sleep(1.0)
    raise RuntimeError("public consumer did not produce a finalized ps5log/1 run")


def close_and_confirm(host: str) -> bool:
    for _ in range(3):
        control("close", host)
        for _ in range(8):
            if running(host) == "none":
                return True
            time.sleep(1.0)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--runs-dir", required=True, type=Path)
    parser.add_argument("--artifact", type=Path,
                        default=ROOT / "dist-consumer/artifact.json")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--texel-rgba8", action="store_true",
                        help="Require the RGBA8 uniform-texel-buffer witness markers")
    parser.add_argument("--texel-formats", action="store_true",
                        help="Require the complete typed uniform-texel format witness")
    parser.add_argument("--cube-array-witness", action="store_true",
                        help="Require the two-cube/six-face sampled-image witness")
    args = parser.parse_args()
    if args.cube_array_witness and (args.texel_rgba8 or args.texel_formats):
        parser.error("Cube-array witness is an independent finite profile")

    if running(args.host) != "none":
        raise RuntimeError("refusing to launch while another title is active")
    known = {path.name for path in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    control("launch", args.host)

    result = {}
    lifecycle_ok = False
    try:
        log = wait_for_log(args.runs_dir, known, args.timeout)
        receipt = json.loads(log.with_suffix(".json").read_text())
        artifact = json.loads(args.artifact.read_text())
        if args.cube_array_witness:
            result = validate_cube_array(log.read_bytes(), receipt, artifact)
        else:
            result = validate(log.read_bytes(), receipt, artifact,
                              texel_rgba8=args.texel_rgba8,
                              texel_formats=args.texel_formats)
        result["source_log"] = str(log)
        result["strict_verified"] = True
    finally:
        lifecycle_ok = close_and_confirm(args.host)
        result["lifecycle_ok"] = lifecycle_ok
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")

    if not lifecycle_ok:
        print("consumer verified but Close Game did not stop the title", file=sys.stderr)
        return 3
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
