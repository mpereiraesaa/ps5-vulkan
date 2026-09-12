#!/usr/bin/env python3
"""Drive one native acceptance run of the focused upstream VK-GL-CTS payload.

The console host is always passed in; nothing about the lab network or identity
is baked into this file. The script:

  1. reads the expected run identity from the built package (selection hash and
     eboot hash written by tools/build_upstream_cts.py),
  2. launches the reserved title through the project's own control helper,
  3. waits for the ps5log/1 run file to finalize on the host,
  4. replays the strict verifier over the captured QPA stream,
  5. closes the title again and confirms it actually stopped.

The CTS verdict and the lifecycle verdict are tracked separately and both are
required: a run whose Close Game did not take is an overall failure even if every
selected upstream case passed.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time
from typing import Any, Dict, Optional

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

EXIT_CTS_FAILURE = 1
EXIT_VERIFICATION = 2
EXIT_LIFECYCLE = 3


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
        raise RunIncomplete(f"unparseable control status output: {status_text!r}")
    return match.group(1)


def close_and_confirm(host: str, title: str, attempts: int = 3,
                      settle: float = 6.0) -> bool:
    """Close the title and require it to actually stop."""
    for attempt in range(attempts):
        control("close", host)
        deadline = time.monotonic() + settle
        while time.monotonic() < deadline:
            if parse_running(control("status", host)) == "none":
                return True
            time.sleep(1.0)
        print(f"[run] close attempt {attempt + 1}/{attempts} did not stop {title}",
              file=sys.stderr)
    return False


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
            f"run did not finalize within {timeout:.0f}s "
            f"(no UPSTREAM_CTS_COMPLETE/BYE): {candidate}")
    raise RunIncomplete(f"no ps5log run file appeared for {title} within {timeout:.0f}s")


def acceptance(args: argparse.Namespace) -> Dict[str, Any]:
    """Run the launch/capture/verify/close cycle and return a combined receipt."""
    expected_selection = (args.dist / "selection_hash.txt").read_text().strip()
    expected_eboot = (args.dist / "eboot_sha256.txt").read_text().strip()
    manifest = json.loads(args.manifest.read_text())

    if args.no_launch:
        # Verifying an existing capture: consider every run file already present.
        known = set()
    else:
        known = {p.name for p in args.runs_dir.glob(f"*_{args.title}_*.log")}
        if parse_running(control("status", args.host)) != "none":
            raise RunIncomplete(f"refusing to launch: {args.title} is already running")
        print(f"[run] launching {args.title}; expected eboot {expected_eboot[:16]}...")
        control("launch", args.host)

    close_required = not args.no_launch and not args.no_close
    cts_verified: Optional[bool] = None
    lifecycle_ok: Optional[bool] = None
    receipt: Dict[str, Any] = {}

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
        cts_verified = verification["strict_verified"]

        print(f"[run] cases={verification['total_reported']} "
              f"pass={verification['pass_count']} fail={verification['fail_count']} "
              f"not_supported={verification['not_supported_count']} "
              f"other={verification['other_count']}")
        for case in verification["case_results"]:
            print(f"       {case['status']:<14} {case['case_path']}")

        receipt = dict(verification)
        receipt["source_log"] = str(log_path)
        receipt["expected_identity"] = {
            "selection_hash": expected_selection,
            "eboot_sha256": expected_eboot,
        }
    finally:
        if close_required:
            lifecycle_ok = close_and_confirm(args.host, args.title,
                                             attempts=args.close_attempts,
                                             settle=args.close_settle)
            print(f"[run] post-close {args.title} stopped={lifecycle_ok}")

        receipt["cts_verified"] = cts_verified
        receipt["lifecycle_ok"] = lifecycle_ok if close_required else None
        receipt["close_required"] = close_required
        if args.out_json:
            args.out_json.parent.mkdir(parents=True, exist_ok=True)
            args.out_json.write_text(json.dumps(receipt, indent=2) + "\n")

    return receipt


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
                        help="leave the title running and do not require lifecycle evidence")
    parser.add_argument("--close-attempts", type=int, default=3,
                        help="Close Game attempts before declaring lifecycle failure")
    parser.add_argument("--close-settle", type=float, default=6.0,
                        help="seconds to wait for the title to stop after each close")
    args = parser.parse_args()

    receipt = acceptance(args)

    cts_ok = bool(receipt.get("cts_verified"))
    lifecycle_ok = receipt.get("lifecycle_ok")

    if lifecycle_ok is False:
        print(f"LIFECYCLE FAILURE: {args.title} was still running after Close Game. "
              "The CTS verdict alone does not constitute acceptance.", file=sys.stderr)
    if not cts_ok:
        print("UPSTREAM ACCEPTANCE FAILED: every selected case must pass its "
              "upstream oracle.", file=sys.stderr)

    if lifecycle_ok is False:
        return EXIT_LIFECYCLE
    if not cts_ok:
        return EXIT_CTS_FAILURE

    print("UPSTREAM ACCEPTANCE PASSED: every selected case matched its upstream "
          "oracle and the title was closed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RunIncomplete as error:
        print(f"VERIFICATION FAILURE: {error}", file=sys.stderr)
        raise SystemExit(EXIT_VERIFICATION)
    except UpstreamVerificationError as error:
        print(f"VERIFICATION FAILURE: {error}", file=sys.stderr)
        raise SystemExit(EXIT_VERIFICATION)
