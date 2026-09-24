"""Run, verify, and close one artifact-bound native gather witness."""
import argparse
import json
from pathlib import Path

try:
    from tools.run_consumer import close_and_confirm, control, running, wait_for_log
    from tools.verify_gather_probe import validate
except ModuleNotFoundError:
    from run_consumer import close_and_confirm, control, running, wait_for_log
    from verify_gather_probe import validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--runs-dir", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()

    if running(args.host) != "none":
        raise RuntimeError("refusing to launch over an active title")
    known = {path.name for path in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    result = {"strict_verified": False, "lifecycle_ok": False}
    try:
        control("launch", args.host)
        log = wait_for_log(args.runs_dir, known, args.timeout)
        result.update(validate(log, args.manifest, args.artifact))
        result["source_log"] = str(log.resolve())
        result["strict_verified"] = True
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        result["lifecycle_ok"] = close_and_confirm(args.host)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")

    print(json.dumps(result, indent=2))
    return 0 if result["strict_verified"] and result["lifecycle_ok"] else 3


if __name__ == "__main__":
    raise SystemExit(main())
