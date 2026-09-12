#!/usr/bin/env python3
"""Strict upstream VK-GL-CTS runner and verifier for PS5 execution.

The parser below models the exact QPA grammar emitted by the pinned framework
(framework/qphelper/qpTestLog.c) and fails closed on anything it does not fully
understand. There is deliberately no "best effort" recovery path: a report that
cannot be parsed structurally is a verification failure, never a pass.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any, Dict, List, Optional, Tuple
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "cts/upstream/manifest.json"

# Status tokens the pinned framework can emit (qpTestLog.c s_qpTestResultMap).
QPA_STATUS_TOKENS = frozenset({
    "Pass", "Fail", "QualityWarning", "CompatibilityWarning", "Pending",
    "NotSupported", "ResourceError", "InternalError", "Crash", "Timeout",
    "Waiver", "DeviceLost",
})

MARKER_BEGIN_SESSION = "#beginSession"
MARKER_END_SESSION = "#endSession"
MARKER_BEGIN_CASE = "#beginTestCaseResult"
MARKER_END_CASE = "#endTestCaseResult"
MARKER_TERMINATE_CASE = "#terminateTestCaseResult"
MARKER_BEGIN_TIMING = "#beginTestsCasesTime"
MARKER_END_TIMING = "#endTestsCasesTime"


class UpstreamVerificationError(Exception):
    """Raised when log integrity or acceptance verification fails."""
    pass


def _parse_case_block(case_path: str, block_lines: List[str]) -> Dict[str, Any]:
    """Parse the XML payload of one normally-terminated test case result."""
    text = "\n".join(block_lines)

    if text.count("<TestCaseResult") != 1:
        raise UpstreamVerificationError(
            f"case {case_path!r}: expected exactly one <TestCaseResult> element")

    start = text.find("<TestCaseResult")
    close = "</TestCaseResult>"
    end = text.rfind(close)
    if end == -1:
        raise UpstreamVerificationError(
            f"case {case_path!r}: <TestCaseResult> is never closed")

    xml_text = text[start:end + len(close)]

    try:
        root = ET.fromstring(xml_text)
    except ET.ParseError as error:
        # No regex fallback: malformed XML can never be salvaged into a status.
        raise UpstreamVerificationError(
            f"case {case_path!r}: malformed result XML ({error})")

    if root.tag != "TestCaseResult":
        raise UpstreamVerificationError(
            f"case {case_path!r}: result root element is {root.tag!r}")

    declared_path = root.get("CasePath")
    if declared_path != case_path:
        raise UpstreamVerificationError(
            f"case {case_path!r}: CasePath attribute says {declared_path!r}")

    result_elements = root.findall("Result")
    if len(result_elements) != 1:
        raise UpstreamVerificationError(
            f"case {case_path!r}: expected exactly one <Result>, found {len(result_elements)}")

    status = result_elements[0].get("StatusCode")
    if not status:
        raise UpstreamVerificationError(
            f"case {case_path!r}: <Result> has no StatusCode")

    details = "".join(result_elements[0].itertext()).strip()
    return {"case_path": case_path, "status": status, "details": details,
            "terminated": False}


def parse_qpa_results(qpa_text: str) -> List[Dict[str, Any]]:
    """Parse the upstream QPA session into per-case results.

    Structural rules enforced (all fatal, none recoverable):

      * exactly one #beginSession and one #endSession, in that order;
      * every #beginTestCaseResult is closed by #endTestCaseResult before the
        session ends, or replaced by #terminateTestCaseResult (crash/timeout);
      * no nested or orphaned case markers;
      * each closed case carries exactly one well-formed <TestCaseResult>
        document whose CasePath matches its marker and whose <Result> carries a
        StatusCode.
    """
    lines = qpa_text.splitlines()

    begin_session = [i for i, line in enumerate(lines)
                     if line.strip() == MARKER_BEGIN_SESSION]
    end_session = [i for i, line in enumerate(lines)
                   if line.strip() == MARKER_END_SESSION]

    if len(begin_session) != 1:
        raise UpstreamVerificationError(
            f"expected exactly one {MARKER_BEGIN_SESSION}, found {len(begin_session)}")
    if len(end_session) != 1:
        raise UpstreamVerificationError(
            f"expected exactly one {MARKER_END_SESSION}, found {len(end_session)}")
    if end_session[0] < begin_session[0]:
        raise UpstreamVerificationError(
            f"{MARKER_END_SESSION} appears before {MARKER_BEGIN_SESSION}")

    results: List[Dict[str, Any]] = []
    open_case: Optional[Dict[str, Any]] = None
    in_timing = False

    for index in range(begin_session[0] + 1, end_session[0]):
        line = lines[index].strip()

        if line == MARKER_BEGIN_TIMING:
            if in_timing:
                raise UpstreamVerificationError("nested case timing section")
            if open_case is not None:
                raise UpstreamVerificationError("case timing section inside a case result")
            in_timing = True
            continue

        if line == MARKER_END_TIMING:
            if not in_timing:
                raise UpstreamVerificationError(
                    f"{MARKER_END_TIMING} without {MARKER_BEGIN_TIMING}")
            in_timing = False
            continue

        if line.startswith(MARKER_BEGIN_CASE):
            if in_timing:
                raise UpstreamVerificationError(
                    "case result inside the case timing section")
            if open_case is not None:
                raise UpstreamVerificationError(
                    f"case {open_case['case_path']!r} was never terminated before {line!r}")
            case_path = line[len(MARKER_BEGIN_CASE):].strip()
            if not case_path or len(case_path.split()) != 1:
                raise UpstreamVerificationError(f"malformed case path in {line!r}")
            open_case = {"case_path": case_path, "lines": []}
            continue

        if line.startswith(MARKER_TERMINATE_CASE):
            if open_case is None:
                raise UpstreamVerificationError(
                    f"{MARKER_TERMINATE_CASE} without an open case result")
            status = line[len(MARKER_TERMINATE_CASE):].strip()
            if not status:
                raise UpstreamVerificationError(
                    f"{MARKER_TERMINATE_CASE} without a result code")
            # An abruptly terminated case never emitted a result: keep it visible
            # as a failure instead of dropping it from the report entirely.
            results.append({"case_path": open_case["case_path"], "status": status,
                            "details": "case terminated abruptly", "terminated": True})
            open_case = None
            continue

        if line == MARKER_END_CASE:
            if open_case is None:
                raise UpstreamVerificationError(f"{MARKER_END_CASE} without an open case")
            results.append(_parse_case_block(open_case["case_path"], open_case["lines"]))
            open_case = None
            continue

        if open_case is not None:
            open_case["lines"].append(lines[index])

    if open_case is not None:
        raise UpstreamVerificationError(
            f"case {open_case['case_path']!r} has no terminator before {MARKER_END_SESSION}")
    if in_timing:
        raise UpstreamVerificationError(
            f"{MARKER_BEGIN_TIMING} is never closed before {MARKER_END_SESSION}")

    return results


def parse_upstream_log_lines(lines: List[str]) -> Tuple[Dict[str, Any], bytes]:
    """Reassemble the verbatim QPA report from the ps5log/1 stream.

    Marker uniqueness and ordering are enforced here; chunks are only legal
    between the start and end markers.
    """
    chunks: Dict[int, bytes] = {}
    chunk_index: Dict[int, int] = {}
    starts: List[Tuple[int, Dict[str, str]]] = []
    ends: List[Tuple[int, Dict[str, Any]]] = []
    completes: List[Tuple[int, int]] = []

    re_start = re.compile(r'UPSTREAM_CTS_START\s+run_id=(\S+)\s+selection_hash=(\S+)\s+eboot_sha256=(\S+)')
    re_chunk = re.compile(r'CHUNK\s+seq=(\d+)\s+size=(\d+)\s+data=(\S+)')
    re_end = re.compile(r'UPSTREAM_CTS_END\s+chunks=(\d+)\s+total_bytes=(\d+)\s+sha256=([0-9a-fA-F]+)')
    re_complete = re.compile(r'UPSTREAM_CTS_COMPLETE\s+status=(-?\d+)')

    for index, raw_line in enumerate(lines):
        line = raw_line.strip()

        match = re_start.search(line)
        if match:
            starts.append((index, {
                "run_id": match.group(1),
                "selection_hash": match.group(2),
                "eboot_sha256": match.group(3),
            }))
            continue

        match = re_chunk.search(line)
        if match:
            seq = int(match.group(1))
            size = int(match.group(2))
            try:
                raw_bytes = base64.b64decode(match.group(3))
            except Exception as error:
                raise UpstreamVerificationError(
                    f"Corrupted base64 in chunk seq={seq}: {error}")
            if len(raw_bytes) != size:
                raise UpstreamVerificationError(
                    f"Chunk seq={seq} size mismatch: expected {size} bytes, got {len(raw_bytes)}")
            if seq in chunks:
                raise UpstreamVerificationError(f"Duplicate chunk seq={seq} received")
            chunks[seq] = raw_bytes
            chunk_index[seq] = index
            continue

        match = re_end.search(line)
        if match:
            ends.append((index, {
                "chunks": int(match.group(1)),
                "total_bytes": int(match.group(2)),
                "sha256": match.group(3).lower(),
            }))
            continue

        match = re_complete.search(line)
        if match:
            completes.append((index, int(match.group(1))))
            continue

    if len(starts) != 1:
        raise UpstreamVerificationError(
            f"expected exactly one UPSTREAM_CTS_START marker, found {len(starts)}")
    if len(ends) != 1:
        raise UpstreamVerificationError(
            f"expected exactly one UPSTREAM_CTS_END marker, found {len(ends)}")
    if len(completes) != 1:
        raise UpstreamVerificationError(
            f"expected exactly one UPSTREAM_CTS_COMPLETE marker, found {len(completes)}; "
            "execution did not finalize cleanly")

    start_index, start_meta = starts[0]
    end_index, end_meta = ends[0]
    complete_index, exit_code = completes[0]

    if not start_index < end_index < complete_index:
        raise UpstreamVerificationError(
            "upstream markers are out of order "
            f"(start={start_index}, end={end_index}, complete={complete_index})")

    for seq, index in chunk_index.items():
        if not start_index < index < end_index:
            raise UpstreamVerificationError(
                f"chunk seq={seq} appears outside the start/end marker window")

    expected_chunk_count = end_meta["chunks"]
    if len(chunks) != expected_chunk_count:
        raise UpstreamVerificationError(
            f"Chunk count mismatch: expected {expected_chunk_count}, received {len(chunks)}")

    qpa_bytes = bytearray()
    for seq in range(expected_chunk_count):
        if seq not in chunks:
            raise UpstreamVerificationError(f"Missing chunk seq={seq} in reassembly stream")
        qpa_bytes.extend(chunks[seq])

    if len(qpa_bytes) != end_meta["total_bytes"]:
        raise UpstreamVerificationError(
            f"Total bytes mismatch: expected {end_meta['total_bytes']}, reassembled {len(qpa_bytes)}")

    actual_sha256 = hashlib.sha256(qpa_bytes).hexdigest().lower()
    if actual_sha256 != end_meta["sha256"]:
        raise UpstreamVerificationError(
            f"SHA-256 mismatch for reassembled QPA report: "
            f"expected {end_meta['sha256']}, got {actual_sha256}")

    metadata = {
        "start": start_meta,
        "end": end_meta,
        "exit_code": exit_code,
        "qpa_sha256": actual_sha256,
        "qpa_bytes_count": len(qpa_bytes),
    }
    return metadata, bytes(qpa_bytes)


def verify_run_identity(metadata: Dict[str, Any], expected: Dict[str, Optional[str]]) -> None:
    """Tie a parsed report to the run/build that was supposed to produce it.

    A report produced by a different selection or a stale executable may still be
    internally consistent, so the expected identity has to be supplied out of
    band (the deployed package's own metadata) and checked here.
    """
    start = metadata["start"]
    problems = []

    for key in ("run_id", "selection_hash", "eboot_sha256"):
        want = expected.get(key)
        if want is not None and start.get(key) != want:
            problems.append(f"{key}: expected {want!r}, report says {start.get(key)!r}")

    if problems:
        raise UpstreamVerificationError("Run identity mismatch: " + "; ".join(problems))


def verify_upstream_acceptance(
    manifest_data: Dict[str, Any],
    metadata: Dict[str, Any],
    results: List[Dict[str, Any]],
) -> Dict[str, Any]:
    """Validate results against the frozen selection manifest.

    Strict acceptance requires a declared and non-empty selection, a zero exit
    status, exactly the declared cases (no missing, extra or duplicate) and a
    ``Pass`` from the upstream oracle for every one of them. An empty selection
    can never be vacuously satisfied.
    """
    expected_cases = [case["path"] for case in manifest_data.get("cases", [])]
    expected_set = set(expected_cases)

    reported_names = [result["case_path"] for result in results]
    seen = set()
    duplicates = []
    for name in reported_names:
        if name in seen:
            duplicates.append(name)
        seen.add(name)

    reported_set = set(reported_names)
    missing = sorted(expected_set - reported_set)
    unexpected = sorted(reported_set - expected_set)

    pass_count = 0
    fail_count = 0
    not_supported_count = 0
    other_count = 0
    acceptance_failures = []
    policy_failures = []

    if not expected_cases:
        policy_failures.append("selection manifest declares no cases")

    for result in results:
        path = result["case_path"]
        status = result["status"]
        if status == "Pass":
            pass_count += 1
        elif status in ("NotSupported", "Not_Supported"):
            not_supported_count += 1
            acceptance_failures.append({"case": path, "expected": "Pass", "actual": status})
        elif status == "Fail":
            fail_count += 1
            acceptance_failures.append({"case": path, "expected": "Pass", "actual": status})
        else:
            other_count += 1
            acceptance_failures.append({"case": path, "expected": "Pass", "actual": status})

    exit_code = metadata.get("exit_code", -1)
    report_valid = (
        exit_code == 0
        and not missing
        and not unexpected
        and not duplicates
        and not policy_failures
    )

    all_required_passed = (
        report_valid
        and not acceptance_failures
        and pass_count == len(expected_cases)
        and len(results) == len(expected_cases)
    )

    return {
        "strict_verified": all_required_passed,
        "report_valid": report_valid,
        "all_required_passed": all_required_passed,
        "exit_code": exit_code,
        "total_expected": len(expected_cases),
        "total_reported": len(results),
        "pass_count": pass_count,
        "fail_count": fail_count,
        "not_supported_count": not_supported_count,
        "other_count": other_count,
        "missing": missing,
        "unexpected": unexpected,
        "duplicates": sorted(set(duplicates)),
        "acceptance_failures": acceptance_failures,
        "policy_failures": policy_failures,
        "metadata": metadata,
        "case_results": results,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Strict Upstream CTS Runner & Verifier")
    parser.add_argument("--log", type=Path, help="Path to raw ps5log stream output file (or stdin if omitted)")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="Path to selection manifest.json")
    parser.add_argument("--out-qpa", type=Path, help="Save reassembled verbatim .qpa report")
    parser.add_argument("--out-json", type=Path, help="Save verification report JSON")
    parser.add_argument("--strict", action="store_true", default=True, help="Enforce strict acceptance exit code")
    parser.add_argument("--expect-run-id", help="Require the report to come from this run id")
    parser.add_argument("--expect-selection-hash", help="Require the deployed case-selection hash")
    parser.add_argument("--expect-eboot-sha256", help="Require the deployed executable hash")
    args = parser.parse_args()

    if args.log and args.log.is_file():
        lines = args.log.read_text(encoding="utf-8", errors="replace").splitlines()
    else:
        lines = sys.stdin.read().splitlines()

    manifest_data = json.loads(args.manifest.read_text())

    try:
        metadata, qpa_bytes = parse_upstream_log_lines(lines)
        verify_run_identity(metadata, {
            "run_id": args.expect_run_id,
            "selection_hash": args.expect_selection_hash,
            "eboot_sha256": args.expect_eboot_sha256,
        })
        results = parse_qpa_results(qpa_bytes.decode("utf-8", errors="replace"))
    except UpstreamVerificationError as error:
        print(f"VERIFICATION FAILURE: {error}", file=sys.stderr)
        sys.exit(2)

    if args.out_qpa:
        args.out_qpa.parent.mkdir(parents=True, exist_ok=True)
        args.out_qpa.write_bytes(qpa_bytes)
        print(f"Reconstructed QPA written to {args.out_qpa} ({len(qpa_bytes)} bytes)")

    verification = verify_upstream_acceptance(manifest_data, metadata, results)

    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(verification, indent=2) + "\n")

    print("Upstream CTS Verification Summary:")
    print(f"  Run ID:           {metadata['start']['run_id']}")
    print(f"  Selection Hash:   {metadata['start']['selection_hash']}")
    print(f"  Eboot SHA256:     {metadata['start']['eboot_sha256']}")
    print(f"  QPA SHA256:       {metadata['qpa_sha256']} ({metadata['qpa_bytes_count']} bytes)")
    print(f"  Cases Expected:   {verification['total_expected']}")
    print(f"  Cases Reported:   {verification['total_reported']}")
    print(f"  Pass:             {verification['pass_count']}")
    print(f"  Fail:             {verification['fail_count']}")
    print(f"  NotSupported:     {verification['not_supported_count']}")
    print(f"  Report Valid:     {verification['report_valid']}")
    print(f"  Strict Verified:  {verification['strict_verified']}")

    if not verification["strict_verified"]:
        for label, key in (("Missing", "missing"), ("Unexpected", "unexpected"),
                           ("Duplicates", "duplicates"), ("Policy", "policy_failures"),
                           ("Acceptance Failures", "acceptance_failures")):
            if verification[key]:
                print(f"  {label}: {verification[key]}", file=sys.stderr)
        if args.strict:
            sys.exit(1)

    print("ALL SELECTED UPSTREAM TESTS VERIFIED AND PASSED STRICT ACCEPTANCE.")


if __name__ == "__main__":
    main()
