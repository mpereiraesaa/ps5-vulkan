"""Run and close a format diagnostic; deployment and mount refresh are separate."""
import argparse
import json
from pathlib import Path

try:
    from tools.run_consumer import close_and_confirm, control, running, wait_for_log
    from tools.verify_sampled_formats import validate as sampled
    from tools.verify_sampled_filtering import validate as filtering
    from tools.verify_integer_sampled_formats import validate as integer
except ModuleNotFoundError:
    from run_consumer import close_and_confirm, control, running, wait_for_log
    from verify_sampled_formats import validate as sampled
    from verify_sampled_filtering import validate as filtering
    from verify_integer_sampled_formats import validate as integer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--runs-dir", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    artifact = json.loads(args.artifact.read_text())
    validators = {7: sampled, 9: filtering, 10: integer}
    if artifact.get("scissor_probe") not in validators:
        raise ValueError("not a format diagnostic artifact")
    if running(args.host) != "none":
        raise RuntimeError("refusing to launch over an active title")
    known = {p.name for p in args.runs_dir.glob("*_PPSA99994_ps5vk_*.log")}
    result = {"strict_verified": False, "deployment_identity_verified": False}
    try:
        control("launch", args.host)
        log = wait_for_log(args.runs_dir, known, args.timeout)
        result["source_log"] = str(log.resolve())
        result.update(validators[artifact["scissor_probe"]](log.read_bytes(),
                      json.loads(log.with_suffix(".json").read_text()), artifact))
        result["strict_verified"] = True
    finally:
        result["process_exit_verified"] = close_and_confirm(args.host)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2))
    return 0 if result["process_exit_verified"] else 3


if __name__ == "__main__":
    raise SystemExit(main())
