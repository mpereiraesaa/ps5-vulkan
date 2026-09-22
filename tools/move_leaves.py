#!/usr/bin/env python3
"""Move leaves between the frozen acceptance selection and its diagnostics.

  move_leaves.py promote --category C [--path P ...] --to-category NEW --receipt R
  move_leaves.py demote  --path P [--category C] --to-category NEW \\
                         --status Fail|NotSupported --reason TEXT

`promote` moves diagnostics into `cases` only when the measurement receipt R
(written by tools/run_upstream_cts.py) reports every one of them as Pass, and
prefixes each rationale with the run and payload that proved it. `demote`
moves cases back into diagnostics with the stated expected status and reason.
Either way the file keeps its formatting, `cases` stays sorted by path, and
the tool prints the counts and the selection hash before and after, plus the
tests that literally pin the old counts, which the same change must update.

--dry-run shows the move without writing; --check then runs the selection gate
and the tests that read the manifest.
"""
import argparse
import datetime as dt
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any, Dict, List

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from make_measurement_manifest import FROZEN_MANIFEST, selection_hash  # noqa: E402

CHECK_MODULES = ["test_upstream_selection", "test_measurement_manifest",
                 "test_upstream_runner", "test_dxvk_backlog", "test_dxvk_matrix"]


class MoveError(Exception):
    pass


def select(entries: List[Dict[str, Any]], categories: List[str],
           paths: List[str]) -> List[Dict[str, Any]]:
    """Entries named by any --path, or in any --category; every name must match."""
    known_paths = {e["path"] for e in entries}
    missing = [p for p in paths if p not in known_paths]
    if missing:
        raise MoveError("not found: " + ", ".join(missing))
    known_categories = {e["category"] for e in entries}
    unknown = [c for c in categories if c not in known_categories]
    if unknown:
        raise MoveError("no entries in category: " + ", ".join(unknown))
    chosen = [e for e in entries if e["path"] in paths or e["category"] in categories]
    if not chosen:
        raise MoveError("nothing selected; pass --category and/or --path")
    return chosen


def receipt_evidence(receipt_path: Path) -> Dict[str, Any]:
    try:
        receipt = json.loads(receipt_path.read_text())
    except (OSError, ValueError) as error:
        raise MoveError(f"cannot read receipt {receipt_path}: {error}")
    statuses = {c["case_path"]: c["status"] for c in receipt.get("case_results", [])}
    if not statuses:
        raise MoveError(f"{receipt_path} has no case_results")
    identity = receipt.get("expected_identity", {})
    eboot = identity.get("eboot_sha256")
    if not eboot:
        raise MoveError(f"{receipt_path} does not name the payload eboot it measured")
    run_id = Path(receipt.get("source_log", "")).stem or receipt_path.stem
    return {"statuses": statuses, "eboot": eboot, "run_id": run_id}


def promote(manifest: Dict[str, Any], categories: List[str], paths: List[str],
            to_category: str, receipt: Dict[str, Any], today: str) -> List[str]:
    chosen = select(manifest["diagnostics"], categories, paths)
    not_passing = [f"{e['path']} ({receipt['statuses'].get(e['path'], 'not in the run')})"
                   for e in chosen if receipt["statuses"].get(e["path"]) != "Pass"]
    if not_passing:
        raise MoveError("the receipt does not show these leaves passing: "
                        + "; ".join(not_passing))
    moved = {e["path"] for e in chosen}
    manifest["diagnostics"] = [e for e in manifest["diagnostics"] if e["path"] not in moved]
    for entry in chosen:
        entry["category"] = to_category
        entry["expected_status"] = "Pass"
        entry["rationale"] = (f"PROMOTED {today}: measured Pass on the physical console in run "
                              f"{receipt['run_id']} on payload eboot {receipt['eboot'][:16]}. "
                              + entry.get("rationale", ""))
    manifest["cases"] = sorted(manifest["cases"] + chosen, key=lambda e: e["path"])
    return sorted(moved)


def demote(manifest: Dict[str, Any], categories: List[str], paths: List[str],
           to_category: str, status: str, reason: str, today: str) -> List[str]:
    chosen = select(manifest["cases"], categories, paths)
    moved = {e["path"] for e in chosen}
    manifest["cases"] = [e for e in manifest["cases"] if e["path"] not in moved]
    for entry in chosen:
        entry["category"] = to_category
        entry["expected_status"] = status
        entry["rationale"] = f"DEMOTED {today}: {reason} " + entry.get("rationale", "")
    manifest["diagnostics"] = manifest["diagnostics"] + chosen
    return sorted(moved)


def pinned_count_hints(old_cases: int, old_diagnostics: int) -> List[str]:
    """Test lines that assert the old acceptance or diagnostic count literally."""
    patterns = [re.compile(rf"\({old_cases},\s*{old_diagnostics}\b"),
                re.compile(rf"assertEqual\(\s*{old_cases}\s*,"),
                re.compile(rf"\b{old_cases} acceptance\b")]
    hints = []
    for path in sorted((ROOT / "tests").glob("test_*.py")):
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if any(p.search(line) for p in patterns):
                hints.append(f"{path.relative_to(ROOT)}:{number}: {line.strip()[:100]}")
    return hints


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("promote", "demote"))
    parser.add_argument("--manifest", type=Path, default=FROZEN_MANIFEST)
    parser.add_argument("--category", action="append", default=[])
    parser.add_argument("--path", action="append", default=[])
    parser.add_argument("--to-category", required=True)
    parser.add_argument("--receipt", type=Path, help="promote: the measurement receipt")
    parser.add_argument("--status", choices=("Fail", "NotSupported"),
                        help="demote: the expected status as a diagnostic")
    parser.add_argument("--reason", help="demote: why the leaf leaves acceptance")
    parser.add_argument("--date", default=dt.date.today().isoformat())
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--check", action="store_true",
                        help="after writing, run the selection gate and its tests")
    args = parser.parse_args(argv)

    manifest = json.loads(args.manifest.read_text())
    before = (len(manifest["cases"]), len(manifest["diagnostics"]),
              selection_hash(manifest["cases"]))
    try:
        if args.action == "promote":
            if not args.receipt:
                raise MoveError("promote needs --receipt: no leaf enters acceptance unmeasured")
            moved = promote(manifest, args.category, args.path, args.to_category,
                            receipt_evidence(args.receipt), args.date)
        else:
            if not (args.status and args.reason):
                raise MoveError("demote needs --status and --reason")
            moved = demote(manifest, args.category, args.path, args.to_category,
                           args.status, args.reason, args.date)
    except MoveError as error:
        print(f"move_leaves: {error}", file=sys.stderr)
        return 2
    after = (len(manifest["cases"]), len(manifest["diagnostics"]),
             selection_hash(manifest["cases"]))

    print(f"{args.action}d {len(moved)} leaves -> category {args.to_category}")
    for path in moved:
        print(f"  {path}")
    print(f"acceptance  {before[0]} -> {after[0]}")
    print(f"diagnostics {before[1]} -> {after[1]}")
    print(f"selection   {before[2][:16]} -> {after[2][:16]}")
    hints = pinned_count_hints(before[0], before[1])
    if hints:
        print("tests that pin the old counts (update them in the same change):")
        for hint in hints:
            print(f"  {hint}")
    if args.dry_run:
        print("dry run: nothing written")
        return 0
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {args.manifest}")
    if args.check:
        gate = subprocess.run([sys.executable, str(ROOT / "tools/check_upstream_selection.py")])
        tests = subprocess.run([sys.executable, str(ROOT / "tools/run_python_tests.py"),
                                *CHECK_MODULES])
        return 1 if gate.returncode or tests.returncode else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
