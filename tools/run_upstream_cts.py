#!/usr/bin/env python3
"""Drive one native acceptance run of the focused upstream VK-GL-CTS payload.

The console host is always passed in; nothing about the lab network or identity
is baked into this file. The script:

  1. reads the expected run identity from the built package (selection hash and
     eboot hash written by tools/build_upstream_cts.py),
  2. launches the reserved title through the project's own control helper,
  3. waits for the ps5log/1 run file to finalize on the host,
  4. replays the strict verifier over the captured QPA stream,
  5. closes the title again and confirms it stopped.

Exit status is 0 only when every declared acceptance case passed the upstream
oracle. A clean-looking but incomplete report is never treated as success.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "cts"))

from upstream_runner import (  # noqa: E402
    UpstreamVerificationError,
    parse_qpa_results,
    parse_upstream_log_lines,
    verify_run_identity,
    verify_upstream_acceptance,
)

DEFAULT_DIST = ROOT / "dist-upstream-cts/PPSA99994"
DEFAULT_MANIFEST = ROOT / "cts/upstream/manifest.json"
CONTROL = ROOT / "tools/control.py"


class RunIncomplete(Exception):
    """A capture started but never produced a finalized upstream report."""


def control(action: str, host: str, timeout: float = 30.0) -> str:
    result = subprocess.run(
        [sys.executable, str(CONTROL), action, "--host", host],
        capture_output=True, text=True, timeout=timeout)
    return (result.stdout or "") + (result.stderr or "")


def parse_running(status_text: str) -> str:
    match = re.search(r"running=(\S+)", status_text)
    if not match:
        raise SystemExit(f"unparseable control status output: {status_text!r}")
    return match.group(1)


def wait_for_finalized_run(runs_dir: Path, title: str, known: set,
                           timeout: float, poll: float = 5.0) -> Path:
    """Return the newest run file for this title that has finished finalizing."""
    deadline = time.monotonic() + timeout
    candidate = None
    while time.monotonic() < deadline:
        logs = sorted(p for p in runs_dir.glob(f"*_{title}_*.log") if p.name not in known)
        if logs:
            candidate = logs[-1]
            text = candidate.read_text(encoding="utf-8", errors="replace")
            if "UPSTREAM_CTS_COMPLETE" in text or "BYE seq=" in text:
                # Give ps5logd a moment to write the JSON manifest and flush.
                time.sleep(3.0)
                return candidate
        time.sleep(poll)
    if candidate is not None:
        raise RunIncomplete(
            f"run did not finalize within {timeout:.0f}s (no UPSTREAM_CTS_COMPLETE/BYE): {candidate}")
    raise SystemExit(f"no ps5log run file appeared for {title} within {timeout:.0f}s")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="console address")
    parser.add_argument("--runs-dir", type=Path, required=True,
                        help="ps5logd runs directory on this host")
    parser.add_argument("--dist", type=Path, default=DEFAULT_DIST,
                        help="built package directory (run identity source)")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--title", default="PPSA99994")
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--out-json", type=Path, help="write the verification receipt here")
    parser.add_argument("--out-qpa", type=Path, help="write the reassembled upstream report here")
    parser.add_argument("--no-launch", action="store_true",
                        help="only verify an already captured run (skip launch/close)")
    parser.add_argument("--no-close", action="store_true",
                        help="leave the title running after the run")
    args = parser.parse_args()

    expected_selection = (args.dist / "selection_hash.txt").read_text().strip()
    expected_eboot = (args.dist / "eboot_sha256.txt").read_text().strip()
    manifest = json.loads(args.manifest.read_text())

    if args.no_launch:
        # Verifying an existing capture: consider every run file already present.
        known = set()
    else:
        known = {p.name for p in args.runs_dir.glob(f"*_{args.title}_*.log")}
        status = control("status", args.host)
        running = parse_running(status)
        if running != "none":
            raise SystemExit(f"refusing to launch: {args.title} already {running}")
        print(f"[run] launching {args.title}; expected eboot {expected_eboot[:16]}...")
        control("launch", args.host)

    try:
        log_path = wait_for_finalized_run(args.runs_dir, args.title, known, args.timeout)
        print(f"[run] captured {log_path}")

        lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        metadata, qpa_bytes = parse_upstream_log_lines(lines)
        verify_run_identity(metadata, {
            "selection_hash": expected_selection,
            "eboot_sha256": expected_eboot,
        })

        if args.out_qpa:
            args.out_qpa.write_bytes(qpa_bytes)
            print(f"[run] wrote reassembled report to {args.out_qpa}")

        results = parse_qpa_results(qpa_bytes.decode("utf-8", errors="replace"))
        verification = verify_upstream_acceptance(manifest, metadata, results)
        verification["source_log"] = str(log_path)
        verification["expected_identity"] = {
            "selection_hash": expected_selection,
            "eboot_sha256": expected_eboot,
        }

        if args.out_json:
            args.out_json.write_text(json.dumps(verification, indent=2) + "\n")

        print(f"[run] cases={verification['total_reported']} "
              f"pass={verification['pass_count']} fail={verification['fail_count']} "
              f"not_supported={verification['not_supported_count']} "
              f"other={verification['other_count']}")
        for case in verification["case_results"]:
            print(f"       {case['status']:<14} {case['case_path']}")

        ok = verification["strict_verified"]
    finally:
        if not args.no_launch and not args.no_close:
            control("close", args.host)
            time.sleep(5)
            state = control("status", args.host)
            running = parse_running(state)
            print(f"[run] post-close {args.title} running={running}")
            if running != "none":
                print("[run] WARNING: title still reported as running", file=sys.stderr)

    if not ok:
        print("UPSTREAM ACCEPTANCE FAILED", file=sys.stderr)
        return 1
    print("UPSTREAM ACCEPTANCE PASSED: every selected case matched its upstream oracle.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RunIncomplete as error:
        print(f"VERIFICATION FAILURE: {error}", file=sys.stderr)
        raise SystemExit(2)
    except UpstreamVerificationError as error:
        print(f"VERIFICATION FAILURE: {error}", file=sys.stderr)
        raise SystemExit(2)
