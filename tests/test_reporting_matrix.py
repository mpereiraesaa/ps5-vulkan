import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import check_reporting_matrix as matrix  # noqa: E402


class TestReportingMatrix(unittest.TestCase):
    def test_committed_matrix_is_current(self):
        """The gate must fail when the reported values move without a refresh."""
        # `make check` builds this dump in its own step and runs the same check;
        # when only the unit suite is run, skip rather than build a second copy.
        if not (ROOT / "build/tests/dump_device_reporting").is_file():
            self.skipTest("reporting dump not built; `make check` builds and runs it")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/check_reporting_matrix.py"), "--check"],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_below_floor_report_without_a_blocker_is_a_violation(self):
        """A new below-floor value must fail the gate, not be silently accepted."""
        core = json.loads((ROOT / "conformance_inventory/core_target.json").read_text())
        row = next(r for r in core["limits"]["rows"]
                   if r["limit"] == "maxPushConstantsSize")
        verdict, _ = matrix.evaluate_limit(row, 128, {})
        self.assertEqual(verdict, "satisfied")
        verdict, _ = matrix.evaluate_limit(row, 127, {})
        self.assertEqual(verdict, "violation")
        self.assertNotIn("maxPushConstantsSize", matrix.KNOWN_BLOCKERS)
        self.assertNotIn("maxDescriptorSetUniformBuffersDynamic", matrix.KNOWN_BLOCKERS)
        self.assertNotIn("maxDescriptorSetStorageBuffersDynamic", matrix.KNOWN_BLOCKERS)
        # Documented blockers are the accepted way to stay below the floor; the
        # mapping happens once, after evaluation, and an unlisted limit would
        # keep its "violation" verdict and fail the gate.
        self.assertIn("maxImageArrayLayers", matrix.KNOWN_BLOCKERS)
        array_row = next(r for r in core["limits"]["rows"]
                         if r["limit"] == "maxImageArrayLayers")
        verdict, _ = matrix.evaluate_limit(array_row, 1, {})
        self.assertEqual(verdict, "violation")

    def test_every_blocker_still_describes_a_real_below_floor_report(self):
        """A blocker entry cannot be used to excuse a value that already passes."""
        data = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        blocked = {row["limit"] for row in data["limits"] if row["verdict"] == "blocker"}
        self.assertTrue(blocked)
        self.assertTrue(blocked <= set(matrix.KNOWN_BLOCKERS))

    def test_feature_gate_citations_resolve(self):
        """Feature rows must cite a code path that is still present."""
        for name, (where, token, _reason) in matrix.FEATURE_GATES.items():
            text = (ROOT / where).read_text()
            self.assertIn(token, text, f"{name} cites a missing token in {where}")
        gate_file, gate_token, test_file, test_token = matrix.FALSE_CORE_FEATURE_GATE
        self.assertIn(gate_token, (ROOT / gate_file).read_text())
        self.assertIn(test_token, (ROOT / test_file).read_text())

    def test_every_false_core_feature_has_a_fail_closed_negotiation_gate(self):
        """A compiler-side feature needs no invented object gate, but it must not enable."""
        data = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        false_rows = [row for row in data["features"] if row["reported"] is False]
        self.assertTrue(false_rows)
        for row in false_rows:
            verdict, detail = matrix.evaluate_feature(row["feature"], False)
            self.assertEqual(verdict, "satisfied", row["feature"])
            self.assertTrue(detail)
        self.assertFalse([row for row in data["features"]
                          if row["verdict"] == "not-audited"])

    def test_advertised_features_are_explicit_and_fail_closed(self):
        """A true bit needs reviewed code evidence and a selected oracle."""
        verdict, _ = matrix.evaluate_feature("robustBufferAccess", True)
        self.assertEqual(verdict, "satisfied")
        verdict, _ = matrix.evaluate_feature("robustBufferAccess", False)
        self.assertEqual(verdict, "violation")
        verdict, _ = matrix.evaluate_feature("shaderInt64", True)
        self.assertEqual(verdict, "violation")
        entry = matrix.ADVERTISED_FEATURES["robustBufferAccess"]
        self.assertEqual(entry["cts"], ("dEQP-VK.info.device_mandatory_features",))
        for where, token in entry["citations"]:
            self.assertIn(token, (ROOT / where).read_text())

    def test_shader_capability_gate_is_mapped(self):
        """Every capability the frontend handles is classified."""
        source = (ROOT / "src/vk_pipeline.c").read_text()
        self.assertIn("spirv_narrow_requirements", source)
        self.assertIn(22, matrix.SHADER_CAPABILITY_ADVERTISEMENT)
        self.assertIn(4433, matrix.SHADER_CAPABILITY_ADVERTISEMENT)

    def test_rgba8_linear_filter_claim_is_exactly_scoped(self):
        """The proven RGBA8 filter bit must not turn unsupported formats green."""
        data = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        rows = [row for row in data["formats"]
                if row.get("feature") == "VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT"]
        rgba8 = [row for row in rows
                 if row.get("profile") == "graphics" and
                 row.get("format") == "VK_FORMAT_R8G8B8A8_UNORM"]
        self.assertEqual(len(rgba8), 1)
        self.assertEqual(rgba8[0]["verdict"], "satisfied")
        self.assertTrue(any(row.get("verdict") == "blocker" and
                            row.get("format") != "VK_FORMAT_R8G8B8A8_UNORM"
                            for row in rows))


if __name__ == "__main__":
    unittest.main()
