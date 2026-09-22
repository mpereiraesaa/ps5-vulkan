#!/usr/bin/env python3
"""Decode one upstream CTS run: per-leaf status, the driver's refusals, a delta.

  decode_run.py RUN [--against OTHER] [--json] [--all]

RUN and OTHER are a ps5log/1 run file (*.log), a reassembled report (*.qpa)
or a receipt written by tools/run_upstream_cts.py (*.json). For a run file,
every driver diagnostic line (a refusal, failure or key marker, or any line
carrying site=/phase=/why=) is attributed to the case that was executing when
it was logged, so each non-passing leaf is shown with the first refusal it
hit. A run that never finalized is decoded leniently: closed cases keep their
status and the case left open is reported as Incomplete.

This is a reading aid. Acceptance is decided only by the strict verifier in
cts/upstream_runner.py.
"""
import argparse
import base64
from collections import Counter
import json
from pathlib import Path
import re
import sys
from typing import Any, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "cts"))

from upstream_runner import (  # noqa: E402
    MARKER_BEGIN_CASE,
    MARKER_END_CASE,
    MARKER_TERMINATE_CASE,
    UpstreamVerificationError,
    _parse_case_block,
    parse_qpa_results,
    parse_upstream_log_lines,
)

CASE_MARK = re.compile(r"UPSTREAM_CTS_CASE\s+name=(\S+)")
DIAGNOSTIC_NAME = re.compile(
    r"\b(PS5VK_[A-Z0-9_]*(?:FAIL|FAILED|REFUSE|REFUSED|REJECT|REJECTED|INVALID|STALL|"
    r"KEY|ERROR|LOST|UNSUPPORTED)[A-Z0-9_]*)\b")
ANY_MARKER = re.compile(r"\b(PS5VK_[A-Z0-9_]+)\b")
SIGNATURE_FIELDS = ("site", "phase", "why", "reason", "stage")
FIELD = re.compile(r"\b(" + "|".join(SIGNATURE_FIELDS) + r")=(\S+)")


def lenient_qpa_results(qpa_text: str) -> List[Dict[str, Any]]:
    """Per-case results from a report that may be truncated or malformed."""
    results: List[Dict[str, Any]] = []
    open_case: Optional[Dict[str, Any]] = None
    for raw in qpa_text.splitlines():
        line = raw.strip()
        if line.startswith(MARKER_BEGIN_CASE):
            if open_case:
                results.append({"case_path": open_case["case_path"], "status": "Incomplete",
                                "details": "no terminator before the next case",
                                "terminated": True})
            open_case = {"case_path": line[len(MARKER_BEGIN_CASE):].strip(), "lines": []}
        elif line.startswith(MARKER_TERMINATE_CASE) and open_case:
            results.append({"case_path": open_case["case_path"],
                            "status": line[len(MARKER_TERMINATE_CASE):].strip() or "Incomplete",
                            "details": "case terminated abruptly", "terminated": True})
            open_case = None
        elif line == MARKER_END_CASE and open_case:
            try:
                results.append(_parse_case_block(open_case["case_path"], open_case["lines"]))
            except UpstreamVerificationError as error:
                results.append({"case_path": open_case["case_path"], "status": "Malformed",
                                "details": str(error), "terminated": True})
            open_case = None
        elif open_case is not None:
            open_case["lines"].append(raw)
    if open_case:
        results.append({"case_path": open_case["case_path"], "status": "Incomplete",
                        "details": "the report ends inside this case", "terminated": True})
    return results


def qpa_from_chunks(lines: List[str]) -> bytes:
    """Best-effort reassembly of whatever chunks a broken run managed to log."""
    chunks: Dict[int, bytes] = {}
    for line in lines:
        match = re.search(r"CHUNK\s+seq=(\d+)\s+size=\d+\s+data=(\S+)", line)
        if match:
            try:
                chunks[int(match.group(1))] = base64.b64decode(match.group(2))
            except ValueError:
                continue
    return b"".join(chunks[key] for key in sorted(chunks))


TEARDOWN = re.compile(r"\bPS5VK_[A-Z0-9_]*(?:RELEASE|DESTROY|FREE)\b")


def diagnostics_by_case(lines: List[str]) -> Tuple[Dict[str, List[str]], Dict[str, str]]:
    """Driver diagnostic lines and the last driver call, keyed by executing case.

    Not every refusal logs a marker (a pipeline refused with
    VK_ERROR_FEATURE_NOT_PRESENT may log only its PS5VK_PIPELINE_CREATE), so
    the last non-teardown driver line of each case is kept as a fallback hint.
    """
    current = "(before the first case)"
    found: Dict[str, List[str]] = {}
    last: Dict[str, str] = {}
    for raw in lines:
        if "CHUNK seq=" in raw:
            continue
        mark = CASE_MARK.search(raw)
        if mark:
            current = mark.group(1)
            continue
        text = raw.split("\t")[-1].strip()
        if not ANY_MARKER.search(text):
            continue
        if DIAGNOSTIC_NAME.search(text) or (FIELD.search(text)
                                             and "PS5VK_GRAPHICS_PHASE" not in text):
            found.setdefault(current, []).append(text)
        if not TEARDOWN.search(text):
            last[current] = text
    return found, last


def signature(text: str) -> str:
    """A diagnostic line reduced to its marker name and its classifying fields."""
    name = DIAGNOSTIC_NAME.search(text) or ANY_MARKER.search(text)
    fields = " ".join(f"{k}={v}" for k, v in FIELD.findall(text))
    return f"{name.group(1) if name else '?'} {fields}".strip()


def load_run(path: Path) -> Dict[str, Any]:
    """Return {"results", "diagnostics", "identity", "complete", "note"} for a run."""
    diagnostics: Dict[str, List[str]] = {}
    last_calls: Dict[str, str] = {}
    identity: Dict[str, Any] = {}
    note = None
    if path.suffix == ".json":
        receipt = json.loads(path.read_text())
        results = [{"case_path": c["case_path"], "status": c["status"],
                    "details": c.get("details", "")} for c in receipt.get("case_results", [])]
        identity = receipt.get("expected_identity", {})
        source = receipt.get("source_log")
        if source and Path(source).is_file():
            diagnostics, last_calls = diagnostics_by_case(Path(source).read_text(
                encoding="utf-8", errors="replace").splitlines())
        return {"results": results, "diagnostics": diagnostics, "last_calls": last_calls,
                "identity": identity, "complete": True, "note": None}
    text = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix == ".qpa":
        qpa = text
    else:
        lines = text.splitlines()
        diagnostics, last_calls = diagnostics_by_case(lines)
        try:
            metadata, qpa_bytes = parse_upstream_log_lines(lines)
            identity = metadata.get("start", {})
        except UpstreamVerificationError as error:
            note = f"run did not finalize ({error}); decoded leniently"
            qpa_bytes = qpa_from_chunks(lines)
        qpa = qpa_bytes.decode("utf-8", errors="replace")
    try:
        results = parse_qpa_results(qpa)
        complete = note is None
    except UpstreamVerificationError as error:
        note = note or f"report is not well formed ({error}); decoded leniently"
        results = lenient_qpa_results(qpa)
        complete = False
    return {"results": results, "diagnostics": diagnostics, "last_calls": last_calls,
            "identity": identity, "complete": complete, "note": note}


def delta(current: List[Dict[str, Any]], previous: List[Dict[str, Any]]) -> Dict[str, Any]:
    now = {r["case_path"]: r["status"] for r in current}
    before = {r["case_path"]: r["status"] for r in previous}
    changed = sorted((path, before[path], now[path]) for path in now.keys() & before.keys()
                     if now[path] != before[path])
    return {
        "fixed": [c for c in changed if c[2] == "Pass"],
        "broke": [c for c in changed if c[1] == "Pass"],
        "moved": [c for c in changed if "Pass" not in (c[1], c[2])],
        "added": sorted(now.keys() - before.keys()),
        "removed": sorted(before.keys() - now.keys()),
    }


def decode(path: Path, against: Optional[Path]) -> Dict[str, Any]:
    run = load_run(path)
    results = run["results"]
    counts = Counter(r["status"] for r in results)
    failing = []
    for result in results:
        if result["status"] == "Pass":
            continue
        lines = run["diagnostics"].get(result["case_path"], [])
        failing.append({**result, "first_refusal": lines[0] if lines else None,
                        "refusal_count": len(lines),
                        "last_driver_call": run["last_calls"].get(result["case_path"])})
    signatures = Counter(signature(line) for case in failing
                         for line in run["diagnostics"].get(case["case_path"], []))
    report = {"run": str(path), "identity": run["identity"], "complete": run["complete"],
              "note": run["note"], "total": len(results), "counts": dict(sorted(counts.items())),
              "failing": failing, "refusal_signatures": signatures.most_common()}
    if against:
        report["against"] = str(against)
        report["delta"] = delta(results, load_run(against)["results"])
    return report


def print_report(report: Dict[str, Any], show_all: bool) -> None:
    identity = report["identity"]
    print(f"run      {report['run']}")
    if identity:
        print(f"identity eboot {str(identity.get('eboot_sha256', '?'))[:16]}  "
              f"selection {str(identity.get('selection_hash', '?'))[:16]}")
    if report["note"]:
        print(f"NOTE     {report['note']}")
    counts = "  ".join(f"{status}={count}" for status, count in report["counts"].items())
    print(f"cases    {report['total']}  {counts}")
    failing = report["failing"]
    limit = None if show_all else 40
    if failing:
        print(f"\nnot passing ({len(failing)}):")
        for case in failing[:limit]:
            detail = case["details"].replace("\n", " ")[:120]
            print(f"  {case['status']:<14} {case['case_path']}")
            if detail:
                print(f"      detail  {detail}")
            if case["first_refusal"]:
                print(f"      refusal {case['first_refusal'][:160]}"
                      + (f"  (+{case['refusal_count'] - 1} more)"
                         if case["refusal_count"] > 1 else ""))
            elif case["last_driver_call"]:
                print(f"      last    {case['last_driver_call'][:160]}  (no refusal marker)")
        if limit and len(failing) > limit:
            print(f"  ... {len(failing) - limit} more (use --all)")
    if report["refusal_signatures"]:
        print("\nrefusal signatures across the failing cases:")
        for text, count in report["refusal_signatures"][:20]:
            print(f"  {count:5}  {text}")
    if "delta" in report:
        d = report["delta"]
        print(f"\ndelta against {report['against']}: {len(d['fixed'])} fixed, "
              f"{len(d['broke'])} broke, {len(d['moved'])} moved, "
              f"{len(d['added'])} added, {len(d['removed'])} removed")
        for label in ("broke", "fixed", "moved"):
            for path, before, now in d[label][:limit]:
                print(f"  {label:<6} {before} -> {now}  {path}")
        for label in ("added", "removed"):
            for path in d[label][:limit]:
                print(f"  {label:<7} {path}")


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run", type=Path)
    parser.add_argument("--against", type=Path, help="an earlier run to diff against")
    parser.add_argument("--json", action="store_true", help="print the report as JSON")
    parser.add_argument("--all", action="store_true", help="do not truncate long lists")
    args = parser.parse_args(argv)
    report = decode(args.run, args.against)
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_report(report, args.all)
    return 0


if __name__ == "__main__":
    sys.exit(main())
