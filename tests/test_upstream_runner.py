import base64
import hashlib
import json
from pathlib import Path
import unittest

from cts.upstream_runner import (
    parse_upstream_log_lines,
    parse_qpa_results,
    verify_upstream_acceptance,
    verify_run_identity,
    UpstreamVerificationError
)

REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = REPO_ROOT / "cts/upstream/manifest.json"

class TestUpstreamRunner(unittest.TestCase):
    def setUp(self):
        self.manifest = {
            "cases": [
                {"path": "dEQP-VK.api.smoke.create_sampler"},
                {"path": "dEQP-VK.api.smoke.create_shader"},
                {"path": "dEQP-VK.compute.basic.shared_var_single_invocation"}
            ]
        }
        self.sample_qpa = (
            "#sessionInfo releaseName 1.3.8.4\n"
            "#beginSession\n"
            "#beginTestCaseResult dEQP-VK.api.smoke.create_sampler\n"
            "<TestCaseResult CasePath=\"dEQP-VK.api.smoke.create_sampler\">\n"
            "  <Result StatusCode=\"Pass\">Creating sampler succeeded</Result>\n"
            "</TestCaseResult>\n"
            "#endTestCaseResult\n"
            "#beginTestCaseResult dEQP-VK.api.smoke.create_shader\n"
            "<TestCaseResult CasePath=\"dEQP-VK.api.smoke.create_shader\">\n"
            "  <Result StatusCode=\"Pass\">Creating shader module succeeded</Result>\n"
            "</TestCaseResult>\n"
            "#endTestCaseResult\n"
            "#beginTestCaseResult dEQP-VK.compute.basic.shared_var_single_invocation\n"
            "<TestCaseResult CasePath=\"dEQP-VK.compute.basic.shared_var_single_invocation\">\n"
            "  <Result StatusCode=\"Pass\">Pass</Result>\n"
            "</TestCaseResult>\n"
            "#endTestCaseResult\n"
            "#endSession\n"
        ).encode("utf-8")

    def _generate_log_stream(self, qpa_bytes: bytes, chunk_size: int = 64, corrupt_chunk_idx: int = -1,
                             drop_chunk_idx: int = -1, duplicate_chunk_idx: int = -1, exit_status: int = 0):
        lines = []
        lines.append("0\t1000\tMARK\tUPSTREAM_CTS_START run_id=test-run selection_hash=abc eboot_sha256=123")

        chunks = [qpa_bytes[i:i + chunk_size] for i in range(0, len(qpa_bytes), chunk_size)]
        for seq, ch in enumerate(chunks):
            if seq == drop_chunk_idx:
                continue
            b64_str = base64.b64encode(ch).decode("ascii")
            if seq == corrupt_chunk_idx:
                b64_str = "!!!NotBase64!!!"
            lines.append(f"0\t1000\tQPA\tCHUNK seq={seq} size={len(ch)} data={b64_str}")
            if seq == duplicate_chunk_idx:
                lines.append(f"0\t1000\tQPA\tCHUNK seq={seq} size={len(ch)} data={b64_str}")

        h = hashlib.sha256(qpa_bytes).hexdigest()
        lines.append(f"0\t1000\tMARK\tUPSTREAM_CTS_END chunks={len(chunks)} total_bytes={len(qpa_bytes)} sha256={h}")
        if exit_status is not None:
            lines.append(f"0\t1000\tINFO\tUPSTREAM_CTS_COMPLETE status={exit_status}")
        return lines

    def test_valid_stream_verification(self):
        lines = self._generate_log_stream(self.sample_qpa)
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        self.assertEqual(qpa_bytes, self.sample_qpa)
        self.assertEqual(meta["start"]["run_id"], "test-run")
        self.assertEqual(meta["exit_code"], 0)

        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        self.assertEqual(len(results), 3)

        v = verify_upstream_acceptance(self.manifest, meta, results)
        self.assertTrue(v["report_valid"])
        self.assertTrue(v["all_required_passed"])
        self.assertTrue(v["strict_verified"])
        self.assertEqual(v["pass_count"], 3)
        self.assertEqual(len(v["missing"]), 0)
        self.assertEqual(len(v["unexpected"]), 0)

    def test_corrupted_base64_raises(self):
        lines = self._generate_log_stream(self.sample_qpa, corrupt_chunk_idx=1)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("Corrupted base64", str(ctx.exception))

    def test_dropped_chunk_raises(self):
        lines = self._generate_log_stream(self.sample_qpa, drop_chunk_idx=2)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("Chunk count mismatch", str(ctx.exception))

    def test_duplicate_chunk_raises(self):
        lines = self._generate_log_stream(self.sample_qpa, duplicate_chunk_idx=1)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("Duplicate chunk", str(ctx.exception))

    def test_missing_completion_raises(self):
        lines = self._generate_log_stream(self.sample_qpa, exit_status=None)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("UPSTREAM_CTS_COMPLETE marker", str(ctx.exception))

    def test_nonzero_exit_status_fails_strict(self):
        lines = self._generate_log_stream(self.sample_qpa, exit_status=1)
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        v = verify_upstream_acceptance(self.manifest, meta, results)
        self.assertFalse(v["report_valid"])
        self.assertFalse(v["strict_verified"])

    def test_not_supported_fails_strict_acceptance(self):
        qpa_unsupported = self.sample_qpa.decode("utf-8").replace(
            "<Result StatusCode=\"Pass\">Creating sampler succeeded</Result>",
            "<Result StatusCode=\"NotSupported\">Unsupported sampler format</Result>"
        ).encode("utf-8")
        lines = self._generate_log_stream(qpa_unsupported)
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        v = verify_upstream_acceptance(self.manifest, meta, results)
        self.assertTrue(v["report_valid"])  # valid stream
        self.assertFalse(v["all_required_passed"])  # but not all required passed
        self.assertFalse(v["strict_verified"])
        self.assertEqual(v["not_supported_count"], 1)
        self.assertEqual(len(v["acceptance_failures"]), 1)

    def test_missing_case_fails_strict_acceptance(self):
        # Manifest expects 3 cases, but QPA only has 2
        manifest_extra = {
            "cases": self.manifest["cases"] + [{"path": "dEQP-VK.memory.missing_test"}]
        }
        lines = self._generate_log_stream(self.sample_qpa)
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        v = verify_upstream_acceptance(manifest_extra, meta, results)
        self.assertFalse(v["report_valid"])
        self.assertFalse(v["strict_verified"])
        self.assertIn("dEQP-VK.memory.missing_test", v["missing"])

    def test_all_unsupported_report_is_not_acceptance(self):
        """An internally valid stream where every case is unsupported is not a pass."""
        qpa = self.sample_qpa.decode("utf-8")
        qpa = qpa.replace('StatusCode="Pass"', 'StatusCode="NotSupported"')
        lines = self._generate_log_stream(qpa.encode("utf-8"))
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        v = verify_upstream_acceptance(self.manifest, meta, results)
        self.assertTrue(v["report_valid"])
        self.assertFalse(v["all_required_passed"])
        self.assertEqual(v["not_supported_count"], 3)
        self.assertEqual(v["pass_count"], 0)

    def test_unknown_status_is_not_acceptance(self):
        qpa = self.sample_qpa.decode("utf-8").replace(
            '<Result StatusCode="Pass">Creating sampler succeeded</Result>',
            '<Result StatusCode="SomethingElse">?</Result>'
        ).encode("utf-8")
        lines = self._generate_log_stream(qpa)
        meta, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        v = verify_upstream_acceptance(self.manifest, meta, results)
        self.assertFalse(v["all_required_passed"])
        self.assertEqual(v["other_count"], 1)

    def test_run_identity_mismatch_is_rejected(self):
        lines = self._generate_log_stream(self.sample_qpa)
        meta, _ = parse_upstream_log_lines(lines)
        # Matching identity passes.
        verify_run_identity(meta, {"run_id": "test-run", "selection_hash": "abc",
                                   "eboot_sha256": "123"})
        with self.assertRaises(UpstreamVerificationError) as ctx:
            verify_run_identity(meta, {"selection_hash": "different"})
        self.assertIn("selection_hash", str(ctx.exception))
        with self.assertRaises(UpstreamVerificationError):
            verify_run_identity(meta, {"eboot_sha256": "stale-binary"})

    # --- structural (fail-closed) parsing -----------------------------------

    def _parse(self, qpa_text):
        lines = self._generate_log_stream(qpa_text.encode("utf-8"))
        metadata, qpa_bytes = parse_upstream_log_lines(lines)
        return parse_qpa_results(qpa_bytes.decode("utf-8"))

    def test_incomplete_xml_is_rejected(self):
        """A missing closing tag must not be salvaged into a Pass status."""
        qpa = self.sample_qpa.decode("utf-8").replace("</TestCaseResult>", "", 1)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            self._parse(qpa)
        self.assertIn("never closed", str(ctx.exception))

    def test_multiple_result_elements_are_rejected(self):
        qpa = self.sample_qpa.decode("utf-8").replace(
            '<Result StatusCode="Pass">Creating sampler succeeded</Result>',
            '<Result StatusCode="Pass">a</Result><Result StatusCode="Fail">b</Result>',
            1)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            self._parse(qpa)
        self.assertIn("exactly one <Result>", str(ctx.exception))

    def test_case_path_mismatch_is_rejected(self):
        qpa = self.sample_qpa.decode("utf-8").replace(
            'CasePath="dEQP-VK.api.smoke.create_sampler"',
            'CasePath="dEQP-VK.api.smoke.something_else"', 1)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            self._parse(qpa)
        self.assertIn("CasePath", str(ctx.exception))

    def test_missing_session_markers_are_rejected(self):
        qpa = (self.sample_qpa.decode("utf-8")
               .replace("#beginSession\n", "")
               .replace("#endSession\n", ""))
        with self.assertRaises(UpstreamVerificationError) as ctx:
            self._parse(qpa)
        self.assertIn("#beginSession", str(ctx.exception))

    def test_unterminated_final_case_is_rejected(self):
        # Drop the terminator of the last case: the session must not accept it.
        qpa = self.sample_qpa.decode("utf-8").replace(
            "#endTestCaseResult\n#endSession", "#endSession")
        with self.assertRaises(UpstreamVerificationError) as ctx:
            self._parse(qpa)
        self.assertIn("no terminator", str(ctx.exception))

    def test_terminated_case_is_not_a_pass(self):
        """#terminateTestCaseResult must surface as a failure, never disappear."""
        qpa = (
            "#beginSession\n"
            "#beginTestCaseResult dEQP-VK.api.smoke.create_sampler\n"
            '<TestCaseResult CasePath="dEQP-VK.api.smoke.create_sampler">\n'
            '  <Result StatusCode="Pass">ok</Result>\n'
            "</TestCaseResult>\n"
            "#endTestCaseResult\n"
            "#beginTestCaseResult dEQP-VK.api.smoke.create_shader\n"
            '<TestCaseResult CasePath="dEQP-VK.api.smoke.create_shader">\n'
            "#terminateTestCaseResult Crash\n"
            "#endSession\n"
        )
        results = self._parse(qpa)
        terminated = [r for r in results if r["case_path"].endswith("create_shader")]
        self.assertEqual(len(terminated), 1)
        self.assertEqual(terminated[0]["status"], "Crash")
        self.assertTrue(terminated[0]["terminated"])

    def test_case_timing_section_is_tolerated(self):
        qpa = self.sample_qpa.decode("utf-8").replace(
            "#beginSession\n",
            "#beginSession\n#beginTestsCasesTime\n"
            "<TestsCasesTime></TestsCasesTime>\n#endTestsCasesTime\n", 1)
        self.assertEqual(len(self._parse(qpa)), 3)

    def test_empty_selection_is_rejected(self):
        """An empty manifest must never satisfy acceptance vacuously."""
        lines = self._generate_log_stream(self.sample_qpa)
        metadata, qpa_bytes = parse_upstream_log_lines(lines)
        results = parse_qpa_results(qpa_bytes.decode("utf-8"))
        verification = verify_upstream_acceptance({"cases": []}, metadata, results)
        self.assertFalse(verification["report_valid"])
        self.assertFalse(verification["strict_verified"])
        self.assertTrue(verification["policy_failures"])

    # --- stream-level marker integrity --------------------------------------

    def test_duplicate_start_marker_is_rejected(self):
        lines = self._generate_log_stream(self.sample_qpa)
        lines.insert(1, lines[0])
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("UPSTREAM_CTS_START", str(ctx.exception))

    def test_duplicate_completion_marker_is_rejected(self):
        lines = self._generate_log_stream(self.sample_qpa)
        lines.append(lines[-1])
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("UPSTREAM_CTS_COMPLETE", str(ctx.exception))

    def test_out_of_order_markers_are_rejected(self):
        lines = self._generate_log_stream(self.sample_qpa)
        complete = lines.pop()
        end_index = next(i for i, line in enumerate(lines) if "UPSTREAM_CTS_END" in line)
        lines.insert(end_index, complete)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("out of order", str(ctx.exception))

    def test_chunk_outside_marker_window_is_rejected(self):
        lines = self._generate_log_stream(self.sample_qpa)
        chunk = lines.pop(1)
        lines.append(chunk)
        with self.assertRaises(UpstreamVerificationError) as ctx:
            parse_upstream_log_lines(lines)
        self.assertIn("outside the start/end marker window", str(ctx.exception))

    def test_selection_manifest_is_wellformed(self):
        manifest = json.loads(MANIFEST_PATH.read_text())
        self.assertEqual(manifest["manifest_version"], "1.0")
        self.assertEqual(manifest["cts_pin"]["tag"], "vulkan-cts-1.3.8.4")
        self.assertEqual(manifest["cts_pin"]["commit"],
                         "a0270c1897597e6c77679870e10415398a13001c")
        self.assertFalse(manifest["acceptance_policy"]["allow_not_supported"])
        self.assertFalse(manifest["acceptance_policy"]["allow_skip"])

        pins = manifest["external_pins"]
        for name in ("glslang", "spirv-tools", "spirv-headers", "amber"):
            self.assertRegex(pins[name], r"^[0-9a-f]{40}$", name)

        paths = [case["path"] for case in manifest["cases"]]
        self.assertEqual(len(paths), len(set(paths)), "duplicate case path in manifest")
        self.assertTrue(paths, "manifest must select at least one case")
        for case in manifest["cases"]:
            self.assertTrue(case["path"].startswith("dEQP-VK."), case["path"])
            self.assertTrue(case["source"], case["path"])
            self.assertEqual(case["expected_status"], "Pass", case["path"])
            self.assertIn("rationale", case)

        # The frozen list must not be the retired synthetic contract suite.
        self.assertFalse(any("contract." in p for p in paths))

    if __name__ == "__main__":
        unittest.main()
