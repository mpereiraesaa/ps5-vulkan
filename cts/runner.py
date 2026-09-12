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
        line = line.split("#")[0].strip()
        if line:
            cases.append(line)
    return cases


HOST_UNSUPPORTED_CASES = {
    "contract.api.smoke.create_sampler",
    "contract.api.smoke.triangle",
    "contract.compute.pipeline.copy_ssbo_single_invocation",
    "contract.compute.pipeline.empty_shader",
}


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
    """Parse [CONTRACT] or [CTS] line formats."""
    results = []
    pattern_contract = re.compile(r'\[CONTRACT\]\s+CASE:\s+(\S+)\s+REF:\s+(\S+)\s+RESULT:\s+(\S+)\s+details=(.*)')
    pattern_cts = re.compile(r'\[(?:CTS|CONTRACT)\]\s+CASE:\s+(\S+)\s+RESULT:\s+(\S+)\s+details=(.*)')
    for line in output.splitlines():
        line = line.strip()
        m_c = pattern_contract.match(line)
        if m_c:
            results.append({
                "name": m_c.group(1),
                "deqp_ref": m_c.group(2),
                "status": m_c.group(3),
                "details": m_c.group(4).strip()
            })
            continue
        m_cts = pattern_cts.match(line)
        if m_cts:
            results.append({
                "name": m_cts.group(1),
                "status": m_cts.group(2),
                "details": m_cts.group(3).strip()
            })
    return results


def validate_results(
    expected_cases: List[str],
    results: List[Dict],
    exit_code: int = 0,
    profile: str = "host"
) -> Dict:
    """Validate report integrity (report_valid) and acceptance expectations (all_required_passed)."""
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

    # 1. Report integrity: the report itself is structurally complete, uncorrupted and legitimate
    report_valid = (
        exit_code == 0
        and len(missing) == 0
        and len(unexpected) == 0
        and len(duplicates) == 0
        and len(invalid_statuses) == 0
        and fail_count == 0
    )

    # 2. Acceptance expectations: explicit expectations per target backend/profile
    acceptance_failures = []
    if profile in ("ps5", "native-ps5", "acceptance"):
        # On native PS5 hardware acceptance, all 26 contract cases must PASS (0 NotSupported, 0 SKIP)
        for r in results:
            if r["status"] != "PASS":
                acceptance_failures.append({
                    "name": r.get("name"),
                    "expected": "PASS",
                    "actual": r.get("status")
                })
        all_required_passed = (
            report_valid
            and (len(acceptance_failures) == 0)
            and (pass_count == len(expected_cases))
            and (not_supported_count == 0)
            and (skip_count == 0)
        )
    elif profile == "host":
        # On host mock, exactly the 4 GPU-dependent cases must be NotSupported and 22 PASS
        for r in results:
            name = r.get("name")
            st = r.get("status")
            if name in HOST_UNSUPPORTED_CASES:
                if st != "NotSupported":
                    acceptance_failures.append({"name": name, "expected": "NotSupported", "actual": st})
            else:
                if st != "PASS":
                    acceptance_failures.append({"name": name, "expected": "PASS", "actual": st})
        all_required_passed = (
            report_valid
            and (len(acceptance_failures) == 0)
            and (pass_count == len(expected_cases) - len(HOST_UNSUPPORTED_CASES))
            and (not_supported_count == len(HOST_UNSUPPORTED_CASES))
            and (skip_count == 0)
        )
    else:
        # Relaxed mode: report validity without profile-specific constraints
        all_required_passed = report_valid and (fail_count == 0)

    ok = report_valid and all_required_passed

    return {
        "profile": profile,
        "total_expected": len(expected_cases),
        "total_reported": len(results),
        "missing": sorted(list(missing)),
        "unexpected": sorted(list(unexpected)),
        "duplicates": sorted(list(set(duplicates))),
        "invalid_statuses": invalid_statuses,
        "acceptance_failures": acceptance_failures,
        "pass": pass_count,
        "not_supported": not_supported_count,
        "fail": fail_count,
        "skip": skip_count,
        "exit_code": exit_code,
        "report_valid": report_valid,
        "all_required_passed": all_required_passed,
        "ok": ok
    }


def verify_gap_matrix(matrix_path: Path, expected_cases: List[str], verbose: bool = True):
    """Verify that all expected contract cases are explicitly documented in the gap matrix."""
    content = matrix_path.read_text()
    missing_from_matrix = []
    for c in expected_cases:
        if c not in content:
            missing_from_matrix.append(c)
    if missing_from_matrix:
        raise AssertionError(f"Gap matrix {matrix_path} is missing coverage for: {missing_from_matrix}")
    if verbose:
        print(f"Gap matrix verified: all {len(expected_cases)} contract cases documented.", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Focused Vulkan contract test runner")
    parser.add_argument("--binary", type=Path, default=DEFAULT_HOST_BIN, help="Test runner binary path")
    parser.add_argument("--case-list", type=Path, default=DEFAULT_CASE_LIST, help="Pinned case list path")
    parser.add_argument("--gap-matrix", type=Path, default=DEFAULT_GAP_MATRIX, help="Gap matrix documentation path")
    parser.add_argument("--profile", choices=["host", "ps5", "relaxed"], default="host", help="Target acceptance profile")
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
        print(f"Host contract binary missing; compiling {DEFAULT_HOST_BIN}...", file=sys.stderr)
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

    val = validate_results(expected_cases, results, exit_code=res.returncode, profile=args.profile)

    if res.returncode != 0:
        print(f"ERROR: Process exited with non-zero exit code {res.returncode}", file=sys.stderr)
    if val["missing"]:
        print(f"ERROR: Missing contract cases from report: {val['missing']}", file=sys.stderr)
    if val["unexpected"]:
        print(f"ERROR: Unexpected/unknown cases in report: {val['unexpected']}", file=sys.stderr)
    if val["duplicates"]:
        print(f"ERROR: Duplicate case results in report: {val['duplicates']}", file=sys.stderr)
    if val["invalid_statuses"]:
        print(f"ERROR: Unrecognized test statuses in report: {val['invalid_statuses']}", file=sys.stderr)
    if val["acceptance_failures"]:
        print(f"ERROR: Acceptance criteria not met for profile '{args.profile}': {val['acceptance_failures']}", file=sys.stderr)

    report_text = ""
    if args.format == "json":
        full_report = {
            "suite": "vulkan-contract-tests",
            "upstream_pin": parsed.get("upstream_pin", "vulkan-cts-1.3.8.4"),
            "validation": val,
            "results": results
        }
        report_text = json.dumps(full_report, indent=2) + "\n"
    elif args.format == "tap":
        lines = [f"1..{len(results)}"]
        for i, r in enumerate(results):
            ref_str = f" (ref: {r.get('deqp_ref')})" if r.get('deqp_ref') else ""
            if r["status"] == "PASS":
                lines.append(f"ok {i + 1} - {r['name']}{ref_str}")
            elif r["status"] == "NotSupported":
                lines.append(f"ok {i + 1} - {r['name']}{ref_str} # SKIP (NotSupported: {r['details']})")
            else:
                lines.append(f"not ok {i + 1} - {r['name']}{ref_str} # {r['details']}")
        report_text = "\n".join(lines) + "\n"
    else:
        lines = [f"=== Vulkan API Contract Tests (profile: {args.profile}, ref: {parsed.get('upstream_pin', 'vulkan-cts-1.3.8.4')}) ==="]
        for r in results:
            lines.append(f"  {r['name']:<52} {r['status']:<12} {r['details']}")
        lines.append(
            f"\nSummary: total={val['total_reported']} pass={val['pass']} "
            f"not_supported={val['not_supported']} fail={val['fail']} skip={val['skip']}\n"
            f"Report Integrity (report_valid): {val['report_valid']} | Profile Acceptance (all_required_passed): {val['all_required_passed']}"
        )
        report_text = "\n".join(lines) + "\n"

    print(report_text, end="")

    if args.out:
        args.out.write_text(report_text)
        print(f"Report written to {args.out}")

    sys.exit(0 if val["ok"] else 1)


if __name__ == "__main__":
    main()
