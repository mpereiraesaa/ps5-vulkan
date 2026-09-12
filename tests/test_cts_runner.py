import json
from pathlib import Path
import subprocess
import unittest

from cts.runner import (
    load_expected_cases,
    parse_cts_json,
    parse_cts_line_output,
    validate_results,
    verify_gap_matrix,
    DEFAULT_CASE_LIST,
    DEFAULT_GAP_MATRIX,
)

ROOT = Path(__file__).resolve().parents[1]


class TestCTSRunner(unittest.TestCase):
    def test_case_list_loading(self):
        cases = load_expected_cases(DEFAULT_CASE_LIST)
        self.assertEqual(len(cases), 26, "Expected exactly 26 pinned CTS test cases")
        self.assertIn("dEQP-VK.info.build", cases)
        self.assertIn("dEQP-VK.synchronization.basic.empty_submit", cases)

    def test_gap_matrix_coverage(self):
        """Programmatically verify that all 26 cases are present in cts/gap_matrix.md."""
        cases = load_expected_cases(DEFAULT_CASE_LIST)
        # verify_gap_matrix raises AssertionError if any case is missing
        verify_gap_matrix(DEFAULT_GAP_MATRIX, cases)

    def test_parse_cts_line_output(self):
        sample = (
            "[CTS] CASE: dEQP-VK.info.build RESULT: PASS details=vulkan 1.0\n"
            "[CTS] CASE: dEQP-VK.info.device RESULT: FAIL details=out of memory\n"
            "[CTS] CASE: dEQP-VK.api.smoke.triangle RESULT: NotSupported details=no hardware\n"
        )
        parsed = parse_cts_line_output(sample)
        self.assertEqual(len(parsed), 3)
        self.assertEqual(parsed[0]["name"], "dEQP-VK.info.build")
        self.assertEqual(parsed[0]["status"], "PASS")
        self.assertEqual(parsed[1]["status"], "FAIL")
        self.assertEqual(parsed[2]["status"], "NotSupported")

    def test_validate_results_complete(self):
        cases = ["case.a", "case.b", "case.c"]
        results = [
            {"name": "case.a", "status": "PASS", "details": ""},
            {"name": "case.b", "status": "NotSupported", "details": "optional feature"},
            {"name": "case.c", "status": "PASS", "details": ""},
        ]
        val = validate_results(cases, results)
        self.assertTrue(val["ok"])
        self.assertEqual(val["pass"], 2)
        self.assertEqual(val["not_supported"], 1)
        self.assertEqual(val["fail"], 0)
        self.assertEqual(len(val["missing"]), 0)
        self.assertEqual(len(val["unexpected"]), 0)

    def test_validate_results_missing_case(self):
        cases = ["case.a", "case.b", "case.c"]
        results = [
            {"name": "case.a", "status": "PASS", "details": ""},
            {"name": "case.b", "status": "PASS", "details": ""},
        ]
        val = validate_results(cases, results)
        self.assertFalse(val["ok"])
        self.assertIn("case.c", val["missing"])

    def test_validate_results_unexpected_case(self):
        cases = ["case.a", "case.b"]
        results = [
            {"name": "case.a", "status": "PASS", "details": ""},
            {"name": "case.b", "status": "PASS", "details": ""},
            {"name": "case.rogue", "status": "PASS", "details": ""},
        ]
        val = validate_results(cases, results)
        self.assertFalse(val["ok"])
        self.assertIn("case.rogue", val["unexpected"])

    def test_validate_results_failure_detected(self):
        cases = ["case.a", "case.b"]
        results = [
            {"name": "case.a", "status": "PASS", "details": ""},
            {"name": "case.b", "status": "FAIL", "details": "assertion failed"},
        ]
        val = validate_results(cases, results)
        self.assertFalse(val["ok"])
        self.assertEqual(val["fail"], 1)

    def test_runner_host_execution(self):
        """Execute runner on host against mock binary and check exit code & validation."""
        cmd = ["python3", str(ROOT / "cts/runner.py"), "--format", "json"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"CTS runner exited with {res.returncode}:\n{res.stderr}")
        data = json.loads(res.stdout)
        self.assertTrue(data["validation"]["ok"])
        self.assertEqual(data["validation"]["total_expected"], 26)
        self.assertEqual(data["validation"]["total_reported"], 26)
        self.assertEqual(data["validation"]["fail"], 0)
        self.assertEqual(data["validation"]["pass"], 22)
        self.assertEqual(data["validation"]["not_supported"], 4)


if __name__ == "__main__":
    unittest.main()
