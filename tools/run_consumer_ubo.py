#!/usr/bin/env python3
"""Run and verify the compact UBO public-SDK consumer, then stop the title."""

import argparse
import json
from pathlib import Path

from run_consumer import close_and_confirm, control, running, wait_for_log
from verify_consumer_ubo_layout import validate


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--runs-dir", required=True, type=Path)
    parser.add_argument("--artifact", type=Path,
                        default=ROOT / "dist-consumer/artifact.json")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()
    if running(args.host) != "none":
        raise RuntimeError("refusing to launch while another title is active")
    known = {path.name for path in
             args.runs_dir.glob("*_PPSA99994_ps5vk-ubo_*.log")}
    control("launch", args.host)

    result = {}
    try:
        log = wait_for_log(args.runs_dir, known, args.timeout, app="ps5vk-ubo")
        receipt = json.loads(log.with_suffix(".json").read_text())
        artifact = json.loads(args.artifact.read_text())
        eboot = args.artifact.parent / "PPSA99994/eboot.bin"
        result = validate(log.read_bytes(), receipt, artifact, eboot)
        result["source_log"] = str(log)
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        result["lifecycle_ok"] = close_and_confirm(args.host)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    if not result["lifecycle_ok"]:
        raise RuntimeError("Close Game did not stop the title")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
