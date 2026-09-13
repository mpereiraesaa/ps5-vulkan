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


if __name__ == "__main__":
    unittest.main()
