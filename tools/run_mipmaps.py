#!/usr/bin/env python3
"""Launch, strictly verify and close one sampled-image mip/LOD-bias PS5 run."""
import argparse
import json
from pathlib import Path

from run_consumer import close_and_confirm, control, running, wait_for_log
from verify_mipmaps import validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--runs-dir", required=True, type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()
    if running(args.host) != "none":
        raise RuntimeError("refusing to launch while another title is active")
    known = {path.name for path in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    control("launch", args.host)
    result = {}
    lifecycle_ok = False
    try:
        log = wait_for_log(args.runs_dir, known, args.timeout)
        receipt = json.loads(log.with_suffix(".json").read_text())
        result = validate(log.read_bytes(), receipt,
                          json.loads(args.artifact.read_text()))
        result.update(source_log=str(log), strict_verified=True)
    finally:
        lifecycle_ok = close_and_confirm(args.host)
        result["lifecycle_ok"] = lifecycle_ok
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result.get("strict_verified") and lifecycle_ok else 3


if __name__ == "__main__":
    raise SystemExit(main())
