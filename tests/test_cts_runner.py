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
        self.assertEqual(len(cases), 26, "Expected exactly 26 pinned contract test cases")
        self.assertIn("contract.info.build", cases)
        self.assertIn("contract.synchronization.empty_submit", cases)

    def test_gap_matrix_coverage(self):
        """Programmatically verify that all 26 cases are present in cts/gap_matrix.md."""
        cases = load_expected_cases(DEFAULT_CASE_LIST)
        # verify_gap_matrix raises AssertionError if any case is missing
        verify_gap_matrix(DEFAULT_GAP_MATRIX, cases)

    def test_parse_cts_line_output(self):
        sample = (
            "[CONTRACT] CASE: contract.info.build REF: dEQP-VK.info.build RESULT: PASS details=vulkan 1.0\n"
            "[CONTRACT] CASE: contract.info.device REF: dEQP-VK.info.device RESULT: FAIL details=out of memory\n"
            "[CONTRACT] CASE: contract.api.smoke.triangle REF: dEQP-VK.api.smoke.triangle RESULT: NotSupported details=no hardware\n"
        )
        parsed = parse_cts_line_output(sample)
        self.assertEqual(len(parsed), 3)
        self.assertEqual(parsed[0]["name"], "contract.info.build")
        self.assertEqual(parsed[0]["deqp_ref"], "dEQP-VK.info.build")
        self.assertEqual(parsed[0]["status"], "PASS")
        self.assertEqual(parsed[1]["status"], "FAIL")
        self.assertEqual(parsed[2]["status"], "NotSupported")

    def test_validate_results_relaxed_informs_integrity_without_acceptance(self):
        cases = ["case.a", "case.b"]
        results = [
            {"name": "case.a", "status": "SKIP", "details": ""},
            {"name": "case.b", "status": "SKIP", "details": ""},
        ]
        val = validate_results(cases, results, profile="relaxed")
        self.assertTrue(val["report_valid"], "Expected report integrity to be valid in relaxed mode")
        self.assertFalse(val["all_required_passed"], "Relaxed mode must not affirm acceptance")
        self.assertTrue(val["ok"], "Overall status for relaxed mode reflects report integrity")
        self.assertEqual(val["skip"], 2)

    def test_validate_results_unknown_profile_rejected(self):
        cases = ["case.a"]
        results = [{"name": "case.a", "status": "PASS", "details": ""}]
        with self.assertRaises(ValueError):
            validate_results(cases, results, profile="native_ps5")
        with self.assertRaises(ValueError):
            validate_results(cases, results, profile="unknown_profile")

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

    def test_validate_results_invalid_status_rejected(self):
        cases = ["case.a", "case.b"]
        results = [
            {"name": "case.a", "status": "PASS", "details": ""},
            {"name": "case.b", "status": "BROKEN", "details": "invented status"},
        ]
        val = validate_results(cases, results)
        self.assertFalse(val["ok"], "Expected validate_results to reject invented status 'BROKEN'")
        self.assertEqual(len(val["invalid_statuses"]), 1)
        self.assertEqual(val["invalid_statuses"][0]["status"], "BROKEN")

    def test_validate_results_duplicate_rejected(self):
        cases = ["case.a", "case.b"]
        results = [
            {"name": "case.a", "status": "PASS", "details": ""},
            {"name": "case.b", "status": "PASS", "details": ""},
            {"name": "case.b", "status": "PASS", "details": "duplicate"},
        ]
        val = validate_results(cases, results)
        self.assertFalse(val["ok"], "Expected validate_results to reject duplicate case results")
        self.assertIn("case.b", val["duplicates"])

    def test_validate_results_nonzero_exit_code_rejected(self):
        cases = ["case.a", "case.b"]
        results = [
            {"name": "case.a", "status": "PASS", "details": ""},
            {"name": "case.b", "status": "PASS", "details": ""},
        ]
        val = validate_results(cases, results, exit_code=1)
        self.assertFalse(val["ok"], "Expected validate_results to reject non-zero process exit code")
        self.assertEqual(val["exit_code"], 1)

    def test_validate_results_profile_acceptance_ps5_rejects_notsupported(self):
        cases = ["case.a", "case.b"]
        results = [
            {"name": "case.a", "status": "PASS", "details": ""},
            {"name": "case.b", "status": "NotSupported", "details": "unsupported"},
        ]
        val = validate_results(cases, results, profile="ps5")
        self.assertTrue(val["report_valid"], "Expected report integrity to be valid")
        self.assertFalse(val["all_required_passed"], "Expected PS5 acceptance to reject NotSupported")
        self.assertFalse(val["ok"])
        self.assertEqual(len(val["acceptance_failures"]), 1)

    def test_validate_results_profile_acceptance_ps5_all_pass(self):
        cases = ["case.a", "case.b"]
        results = [
            {"name": "case.a", "status": "PASS", "details": ""},
            {"name": "case.b", "status": "PASS", "details": ""},
        ]
        val = validate_results(cases, results, profile="ps5")
        self.assertTrue(val["report_valid"])
        self.assertTrue(val["all_required_passed"])
        self.assertTrue(val["ok"])

    def test_runner_host_execution(self):
        """Execute runner on host against mock binary and check exit code & validation."""
        cmd = ["python3", str(ROOT / "cts/runner.py"), "--format", "json"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"CTS runner exited with {res.returncode}:\n{res.stderr}")
        data = json.loads(res.stdout)
        self.assertTrue(data["validation"]["report_valid"])
        self.assertTrue(data["validation"]["all_required_passed"])
        self.assertTrue(data["validation"]["ok"])
        self.assertEqual(data["validation"]["total_expected"], 26)
        self.assertEqual(data["validation"]["total_reported"], 26)
        self.assertEqual(data["validation"]["fail"], 0)
        self.assertEqual(data["validation"]["pass"], 22)
        self.assertEqual(data["validation"]["not_supported"], 4)


if __name__ == "__main__":
    unittest.main()
