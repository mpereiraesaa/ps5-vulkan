#!/usr/bin/env python3
"""Focused Vulkan CTS runner for host simulation and native PS5."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Dict, List, Optional

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CASE_LIST = ROOT / "cts/case_list.txt"
DEFAULT_HOST_BIN = ROOT / "build/tests/test_cts_host"
DEFAULT_GAP_MATRIX = ROOT / "cts/gap_matrix.md"


def load_expected_cases(case_list_path: Path) -> List[str]:
    cases = []
    for line in case_list_path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            cases.append(line)
    return cases


def parse_cts_json(output: str) -> Dict:
    """Parse machine-readable JSON output from cts_adapter."""
    try:
        return json.loads(output)
    except json.JSONDecodeError as e:
        # If there's leading text before the JSON block
        json_start = output.find("{")
        if json_start != -1:
            try:
                return json.loads(output[json_start:])
            except Exception:
                pass
        raise ValueError(f"Failed to parse CTS JSON output: {e}\nRaw output:\n{output}")


def parse_cts_line_output(output: str) -> List[Dict]:
    """Parse [CTS] CASE: ... RESULT: ... line format."""
    results = []
    pattern = re.compile(r'\[CTS\]\s+CASE:\s+(\S+)\s+RESULT:\s+(\S+)\s+details=(.*)')
    for line in output.splitlines():
        m = pattern.match(line.strip())
        if m:
            results.append({
                "name": m.group(1),
                "status": m.group(2),
                "details": m.group(3).strip()
            })
    return results


def validate_results(expected_cases: List[str], results: List[Dict], exit_code: int = 0) -> Dict:
    """Ensure every case in case_list was reported with no omissions, duplicates, or invalid statuses."""
    expected_set = set(expected_cases)

    reported_names = []
    duplicates = []
    seen = set()
    for r in results:
        name = r.get("name", "")
        reported_names.append(name)
        if name in seen:
            duplicates.append(name)
        seen.add(name)

    reported_set = set(reported_names)
    missing = expected_set - reported_set
    unexpected = reported_set - expected_set

    ALLOWED_STATUSES = {"PASS", "NotSupported", "SKIP"}

    pass_count = 0
    not_supported_count = 0
    fail_count = 0
    skip_count = 0
    invalid_statuses = []

    for r in results:
        st = r.get("status", "")
        if st == "PASS":
            pass_count += 1
        elif st == "NotSupported":
            not_supported_count += 1
        elif st == "SKIP":
            skip_count += 1
        elif st == "FAIL":
            fail_count += 1
        else:
            invalid_statuses.append({"name": r.get("name"), "status": st})

    ok = (
        len(missing) == 0
        and len(unexpected) == 0
        and len(duplicates) == 0
        and len(invalid_statuses) == 0
        and fail_count == 0
        and exit_code == 0
    )

    return {
        "total_expected": len(expected_cases),
        "total_reported": len(results),
        "missing": sorted(list(missing)),
        "unexpected": sorted(list(unexpected)),
        "duplicates": sorted(list(set(duplicates))),
        "invalid_statuses": invalid_statuses,
        "pass": pass_count,
        "not_supported": not_supported_count,
        "fail": fail_count,
        "skip": skip_count,
        "exit_code": exit_code,
        "ok": ok
    }


def verify_gap_matrix(matrix_path: Path, expected_cases: List[str], verbose: bool = True):
    """Verify that all expected CTS cases are explicitly documented in the gap matrix."""
    content = matrix_path.read_text()
    missing_from_matrix = []
    for c in expected_cases:
        if c not in content:
            missing_from_matrix.append(c)
    if missing_from_matrix:
        raise AssertionError(f"Gap matrix {matrix_path} is missing coverage for: {missing_from_matrix}")
    if verbose:
        print(f"Gap matrix verified: all {len(expected_cases)} CTS cases documented.", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Focused Vulkan CTS runner")
    parser.add_argument("--binary", type=Path, default=DEFAULT_HOST_BIN, help="CTS runner binary path")
    parser.add_argument("--case-list", type=Path, default=DEFAULT_CASE_LIST, help="Pinned CTS case list path")
    parser.add_argument("--gap-matrix", type=Path, default=DEFAULT_GAP_MATRIX, help="Gap matrix documentation path")
    parser.add_argument("--format", choices=["summary", "json", "tap"], default="summary", help="Output format")
    parser.add_argument("--out", type=Path, help="Write output report to file")
    parser.add_argument("--check-matrix-only", action="store_true", help="Only verify gap matrix coverage")
    args = parser.parse_args()

    expected_cases = load_expected_cases(args.case_list)

    if args.gap_matrix.is_file():
        verify_gap_matrix(args.gap_matrix, expected_cases)

    if args.check_matrix_only:
        return

    # Ensure host binary is built if needed
    if not args.binary.is_file() and args.binary == DEFAULT_HOST_BIN:
        if not (ROOT / "dist-sdk/lib/libps5vk_host.a").is_file():
            print("Staged SDK missing; running tools/build_sdk.py...", file=sys.stderr)
            subprocess.run([sys.executable, str(ROOT / "tools/build_sdk.py")], check=True)
        print(f"Host CTS binary missing; compiling {DEFAULT_HOST_BIN}...", file=sys.stderr)
        DEFAULT_HOST_BIN.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "dist-sdk/include"),
            "-I" + str(ROOT / "cts"),
            str(ROOT / "cts/cts_adapter.c"),
            str(ROOT / "dist-sdk/lib/libps5vk_host.a"),
            "-o", str(DEFAULT_HOST_BIN)
        ], check=True)

    # Execute binary with --json for reliable machine parsing
    res = subprocess.run([str(args.binary), "--json"], capture_output=True, text=True)
    try:
        parsed = parse_cts_json(res.stdout)
    except Exception as e:
        parsed = {"results": [], "error": str(e)}
    results = parsed.get("results", [])

    val = validate_results(expected_cases, results, exit_code=res.returncode)

    if res.returncode != 0:
        print(f"ERROR: Process exited with non-zero exit code {res.returncode}", file=sys.stderr)
    if val["missing"]:
        print(f"ERROR: Missing CTS cases from report: {val['missing']}", file=sys.stderr)
    if val["unexpected"]:
        print(f"ERROR: Unexpected/unknown CTS cases in report: {val['unexpected']}", file=sys.stderr)
    if val["duplicates"]:
        print(f"ERROR: Duplicate CTS case results in report: {val['duplicates']}", file=sys.stderr)
    if val["invalid_statuses"]:
        print(f"ERROR: Unrecognized test statuses in report: {val['invalid_statuses']}", file=sys.stderr)

    report_text = ""
    if args.format == "json":
        full_report = {
            "upstream_pin": parsed.get("upstream_pin", "vulkan-cts-1.3.8.4"),
            "validation": val,
            "results": results
        }
        report_text = json.dumps(full_report, indent=2) + "\n"
    elif args.format == "tap":
        lines = [f"1..{len(results)}"]
        for i, r in enumerate(results):
            if r["status"] == "PASS":
                lines.append(f"ok {i + 1} - {r['name']}")
            elif r["status"] == "NotSupported":
                lines.append(f"ok {i + 1} - {r['name']} # SKIP (NotSupported: {r['details']})")
            else:
                lines.append(f"not ok {i + 1} - {r['name']} # {r['details']}")
        report_text = "\n".join(lines) + "\n"
    else:
        lines = [f"=== Vulkan CTS Focused Run ({parsed.get('upstream_pin', 'vulkan-cts-1.3.8.4')}) ==="]
        for r in results:
            lines.append(f"  {r['name']:<58} {r['status']:<12} {r['details']}")
        lines.append(
            f"\nSummary: total={val['total_reported']} pass={val['pass']} "
            f"not_supported={val['not_supported']} fail={val['fail']} skip={val['skip']}"
        )
        report_text = "\n".join(lines) + "\n"

    print(report_text, end="")

    if args.out:
        args.out.write_text(report_text)
        print(f"Report written to {args.out}")

    sys.exit(0 if val["ok"] else 1)


if __name__ == "__main__":
    main()
