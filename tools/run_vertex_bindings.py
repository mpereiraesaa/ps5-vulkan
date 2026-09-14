"""Run the vertex-binding witness; deployment and mount refresh are separate."""
import argparse
import json
from pathlib import Path

from run_consumer import close_and_confirm, control, running, wait_for_log
from verify_vertex_bindings import validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--runs-dir", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    if running(args.host) != "none":
        raise RuntimeError("refusing to launch over an active title")
    known = {p.name for p in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    result = {}
    try:
        control("launch", args.host)
        log = wait_for_log(args.runs_dir, known, args.timeout)
        result = validate(log.read_bytes(),
                          json.loads(log.with_suffix(".json").read_text()),
                          json.loads(args.artifact.read_text()))
        result["source_log"] = str(log.resolve())
        result["strict_verified"] = True
    finally:
        result["process_exit_verified"] = close_and_confirm(args.host)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2))
    return 0 if result["process_exit_verified"] else 3


if __name__ == "__main__":
    raise SystemExit(main())
