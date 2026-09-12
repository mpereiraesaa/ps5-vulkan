#!/usr/bin/env python3
"""Strict upstream VK-GL-CTS runner and verifier for PS5 execution."""
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

class UpstreamVerificationError(Exception):
    """Raised when log integrity or acceptance verification fails."""
    pass

def parse_upstream_log_lines(lines: List[str]) -> Tuple[Dict[str, Any], bytes]:
    """
    Parse ps5log stream lines, reconstruct the verbatim QPA report from base64 chunks,
    and verify chunk sequence and cryptographic checksums.
    """
    start_meta = None
    end_meta = None
    chunks = {}
    completed_status = None

    re_start = re.compile(r'UPSTREAM_CTS_START\s+run_id=(\S+)\s+selection_hash=(\S+)\s+eboot_sha256=(\S+)')
    re_chunk = re.compile(r'CHUNK\s+seq=(\d+)\s+size=(\d+)\s+data=(\S+)')
    re_end = re.compile(r'UPSTREAM_CTS_END\s+chunks=(\d+)\s+total_bytes=(\d+)\s+sha256=([0-9a-fA-F]+)')
    re_complete = re.compile(r'UPSTREAM_CTS_COMPLETE\s+status=(-?\d+)')

    for raw_line in lines:
        line = raw_line.strip()
        # Look for markers
        m_start = re_start.search(line)
        if m_start:
            start_meta = {
                "run_id": m_start.group(1),
                "selection_hash": m_start.group(2),
                "eboot_sha256": m_start.group(3)
            }
            continue

        m_chunk = re_chunk.search(line)
        if m_chunk:
            seq = int(m_chunk.group(1))
            size = int(m_chunk.group(2))
            b64_data = m_chunk.group(3)
            try:
                raw_bytes = base64.b64decode(b64_data)
            except Exception as e:
                raise UpstreamVerificationError(f"Corrupted base64 in chunk seq={seq}: {e}")
            if len(raw_bytes) != size:
                raise UpstreamVerificationError(f"Chunk seq={seq} size mismatch: expected {size} bytes, got {len(raw_bytes)}")
            if seq in chunks:
                raise UpstreamVerificationError(f"Duplicate chunk seq={seq} received")
            chunks[seq] = raw_bytes
            continue

        m_end = re_end.search(line)
        if m_end:
            end_meta = {
                "chunks": int(m_end.group(1)),
                "total_bytes": int(m_end.group(2)),
                "sha256": m_end.group(3).lower()
            }
            continue

        m_comp = re_complete.search(line)
        if m_comp:
            completed_status = int(m_comp.group(1))
            continue

    if not start_meta:
        raise UpstreamVerificationError("Missing UPSTREAM_CTS_START marker in log stream")
    if not end_meta:
        raise UpstreamVerificationError("Missing UPSTREAM_CTS_END marker in log stream")
    if completed_status is None:
        raise UpstreamVerificationError("Missing UPSTREAM_CTS_COMPLETE marker; execution did not finalize cleanly")

    # Verify chunk sequence
    expected_chunk_count = end_meta["chunks"]
    if len(chunks) != expected_chunk_count:
        raise UpstreamVerificationError(
            f"Chunk count mismatch: expected {expected_chunk_count}, received {len(chunks)}"
        )

    qpa_bytes = bytearray()
    for seq in range(expected_chunk_count):
        if seq not in chunks:
            raise UpstreamVerificationError(f"Missing chunk seq={seq} in reassembly stream")
        qpa_bytes.extend(chunks[seq])

    if len(qpa_bytes) != end_meta["total_bytes"]:
        raise UpstreamVerificationError(
            f"Total bytes mismatch: expected {end_meta['total_bytes']}, reassembled {len(qpa_bytes)}"
        )

    actual_sha256 = hashlib.sha256(qpa_bytes).hexdigest().lower()
    if actual_sha256 != end_meta["sha256"]:
        raise UpstreamVerificationError(
            f"SHA-256 mismatch for reassembled QPA report: expected {end_meta['sha256']}, got {actual_sha256}"
        )

    metadata = {
        "start": start_meta,
        "end": end_meta,
        "exit_code": completed_status,
        "qpa_sha256": actual_sha256,
        "qpa_bytes_count": len(qpa_bytes),
    }

    return metadata, bytes(qpa_bytes)


def parse_qpa_results(qpa_text: str) -> List[Dict[str, str]]:
    """
    Parse test case results from reconstructed QPA stream.
    QPA encapsulates each test inside #beginTestCaseResult <path> ... #endTestCaseResult.
    Within each block, an XML <TestCaseResult> structure is parsed.
    """
    results = []
    blocks = re.findall(r'#beginTestCaseResult\s+(\S+)(.*?)(?:#endTestCaseResult|\Z)', qpa_text, re.DOTALL)

    for case_path, block_content in blocks:
        case_path = case_path.strip()
        # Find XML block inside
        xml_start = block_content.find("<TestCaseResult")
        xml_end = block_content.rfind("</TestCaseResult>")
        status = "Unknown"
        details = ""

        if xml_start != -1 and xml_end != -1:
            xml_str = block_content[xml_start : xml_end + len("</TestCaseResult>")]
            try:
                root = ET.fromstring(xml_str)
                res_elem = root.find("Result")
                if res_elem is not None:
                    status = res_elem.get("StatusCode", "Unknown")
                    details = (res_elem.text or "").strip()
            except ET.ParseError as e:
                # If XML snippet cannot be parsed directly, regex fallback on <Result StatusCode="...">
                m_res = re.search(r'<Result\s+StatusCode="([^"]+)"[^>]*>(.*?)</Result>', block_content, re.DOTALL)
                if m_res:
                    status = m_res.group(1)
                    details = m_res.group(2).strip()
                else:
                    details = f"XML Parse Error: {e}"
        else:
            # Fallback regex search
            m_res = re.search(r'<Result\s+StatusCode="([^"]+)"[^>]*>(.*?)</Result>', block_content, re.DOTALL)
            if m_res:
                status = m_res.group(1)
                details = m_res.group(2).strip()

        results.append({
            "case_path": case_path,
            "status": status,
            "details": details
        })

    return results


def verify_run_identity(metadata: Dict[str, Any], expected: Dict[str, Optional[str]]) -> None:
    """
    Tie a parsed report to the run/build that was supposed to produce it.

    A report that was produced by a different selection or a stale executable may
    still be internally consistent, so the expected identity has to be supplied
    out of band (the deployed package's own metadata) and checked here.
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
    results: List[Dict[str, str]]
) -> Dict[str, Any]:
    """
    Validate test results against frozen selection manifest.
    Strict acceptance enforces:
    - Exit code must be 0
    - All expected cases must be present
    - No unexpected cases
    - No duplicate cases
    - Every case must have status == "Pass" (case-insensitive "pass" accepted as match)
    """
    expected_cases = [c["path"] for c in manifest_data["cases"]]
    expected_set = set(expected_cases)

    reported_names = [r["case_path"] for r in results]
    seen = set()
    duplicates = []
    for name in reported_names:
        if name in seen:
            duplicates.append(name)
        seen.add(name)

    reported_set = set(reported_names)
    missing = sorted(list(expected_set - reported_set))
    unexpected = sorted(list(reported_set - expected_set))

    pass_count = 0
    fail_count = 0
    not_supported_count = 0
    other_count = 0
    acceptance_failures = []

    for r in results:
        path = r["case_path"]
        st = r["status"]
        if st.lower() == "pass":
            pass_count += 1
        elif st.lower() in ("notsupported", "not_supported"):
            not_supported_count += 1
            acceptance_failures.append({"case": path, "expected": "Pass", "actual": st})
        elif st.lower() == "fail":
            fail_count += 1
            acceptance_failures.append({"case": path, "expected": "Pass", "actual": st})
        else:
            other_count += 1
            acceptance_failures.append({"case": path, "expected": "Pass", "actual": st})

    exit_code = metadata.get("exit_code", -1)
    report_valid = (
        exit_code == 0
        and len(missing) == 0
        and len(unexpected) == 0
        and len(duplicates) == 0
    )

    all_required_passed = (
        report_valid
        and len(acceptance_failures) == 0
        and pass_count == len(expected_cases)
        and not_supported_count == 0
        and fail_count == 0
        and other_count == 0
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
        "duplicates": sorted(list(set(duplicates))),
        "acceptance_failures": acceptance_failures,
        "metadata": metadata,
        "case_results": results,
    }


def main():
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
    except UpstreamVerificationError as e:
        print(f"VERIFICATION FAILURE: {e}", file=sys.stderr)
        sys.exit(2)

    try:
        verify_run_identity(metadata, {
            "run_id": args.expect_run_id,
            "selection_hash": args.expect_selection_hash,
            "eboot_sha256": args.expect_eboot_sha256,
        })
    except UpstreamVerificationError as e:
        print(f"VERIFICATION FAILURE: {e}", file=sys.stderr)
        sys.exit(3)

    if args.out_qpa:
        args.out_qpa.parent.mkdir(parents=True, exist_ok=True)
        args.out_qpa.write_bytes(qpa_bytes)
        print(f"Reconstructed QPA written to {args.out_qpa} ({len(qpa_bytes)} bytes)")

    qpa_text = qpa_bytes.decode("utf-8", errors="replace")
    results = parse_qpa_results(qpa_text)
    verification = verify_upstream_acceptance(manifest_data, metadata, results)

    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(verification, indent=2) + "\n")

    print(f"Upstream CTS Verification Summary:")
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
        if verification["missing"]:
            print(f"  Missing: {verification['missing']}", file=sys.stderr)
        if verification["unexpected"]:
            print(f"  Unexpected: {verification['unexpected']}", file=sys.stderr)
        if verification["duplicates"]:
            print(f"  Duplicates: {verification['duplicates']}", file=sys.stderr)
        if verification["acceptance_failures"]:
            print(f"  Acceptance Failures: {verification['acceptance_failures']}", file=sys.stderr)
        if args.strict:
            sys.exit(1)

    print("ALL SELECTED UPSTREAM TESTS VERIFIED AND PASSED STRICT ACCEPTANCE.")

if __name__ == "__main__":
    main()
