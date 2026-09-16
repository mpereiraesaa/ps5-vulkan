import copy
import importlib.util
import json
import tempfile
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


derive = load_tool("derive_dxvk_profile")
matrix = load_tool("check_dxvk_profile")


class DxvkMatrixTests(unittest.TestCase):
    def test_multiview_floors_are_private_evidence_on_every_row(self):
        """The two multiview floors are measured natively and attached to the
        feature AND to both properties, with the run that measured each one - and
        none of the three may turn into implementation or CTS evidence, or fall
        back to 'unmeasured', while the capability stays unpromoted."""
        document = matrix.generate()
        six_view_run = "20260916T050841017Z_PPSA99994_ps5vk_0x11321e8ad91f2"
        six_view_artifact = "92a4073e227f28028e6a57a4a8d14f8e829bdc25c21e7e3ceb5a3c42f33c3c01"
        instance_run = "20260916T061619722Z_PPSA99994_ps5vk_0x116d2e36312a6"
        instance_artifact = "0d2654c10fd727a2a63b5e3ae23e42a94b83529d4abc6ffe3dde7653411cc1da"
        rows = {row["id"]: row for row in document["requirements"]}
        expected = {
            "feature:VkPhysicalDeviceVulkan11Features:multiview":
                ({six_view_run, instance_run}, {six_view_artifact, instance_artifact}),
            "property:VkPhysicalDeviceVulkan11Properties:maxMultiviewViewCount":
                ({six_view_run}, {six_view_artifact}),
            "property:VkPhysicalDeviceVulkan11Properties:maxMultiviewInstanceIndex":
                ({instance_run}, {instance_artifact}),
        }
        for identifier, (runs, artifacts) in expected.items():
            row = rows[identifier]
            self.assertEqual("native-evidence", row["native"]["state"], identifier)
            self.assertEqual(runs, set(row["native"]["run_ids"]), identifier)
            self.assertIn(row["native"]["artifact_sha256"], artifacts, identifier)
            self.assertNotEqual("not-run", row["native"]["state"], identifier)
            # Never implementation or CTS evidence: the capability is unpromoted.
            self.assertEqual("missing", row["implementation"]["state"], identifier)
            self.assertEqual("not-mapped", row["cts"]["state"], identifier)
        # Both properties name exactly the run that measured their floor.
        self.assertNotEqual(
            rows["property:VkPhysicalDeviceVulkan11Properties:maxMultiviewViewCount"]["native"],
            rows["property:VkPhysicalDeviceVulkan11Properties:maxMultiviewInstanceIndex"]["native"])
        # ...and the profile is no readier than before: same ready count, no CTS
        # or implementation movement.
        self.assertEqual(1, document["summary"]["satisfied"])
        self.assertEqual(61, document["summary"]["blocker"])

    def test_matrix_is_exhaustive_and_fail_closed(self):
        document = matrix.generate()
        matrix.validate(document)
        profile = json.loads(derive.OUTPUT.read_text())
        self.assertEqual([row["id"] for row in profile["requirements"]],
                         [row["id"] for row in document["requirements"]])
        self.assertEqual(62, document["summary"]["requirements"])
        self.assertEqual(1, document["summary"]["satisfied"])
        self.assertEqual(61, document["summary"]["blocker"])
        self.assertEqual(
            ["feature:VkPhysicalDeviceFeatures:robustBufferAccess"],
            [row["id"] for row in document["requirements"]
             if row["verdict"] == "satisfied"])
        self.assertNotIn("not-run",
                         {row["native"]["state"] for row in document["requirements"]})

    def test_capability_probe_accepts_one_strict_run(self):
        """One strict run is evidence; zero runs is not.

        The owner workflow removed the redundant two-identical-run policy, so
        the receipt must accept a single well-formed run - and every row that
        cites the capability probe must then name that one run and its
        artifact, never a mixture of artifacts."""
        evidence = json.loads(matrix.EVIDENCE.read_text())
        original = matrix.EVIDENCE
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "evidence.json"
            matrix.EVIDENCE = path
            try:
                broken = copy.deepcopy(evidence)
                broken["capability_probe"]["runs"] = []
                path.write_text(json.dumps(broken))
                with self.assertRaisesRegex(ValueError, "capability-probe evidence"):
                    matrix.generate()

                single = copy.deepcopy(evidence)
                single["capability_probe"]["runs"] = single["capability_probe"]["runs"][:1]
                path.write_text(json.dumps(single))
                document = matrix.generate()
                # Every row that takes its evidence FROM the probe (same
                # artifact) must cite exactly that one run: no row may keep an
                # older run beside the current artifact. Rows with their own
                # override keep their own artifacts.
                cited = [row for row in document["requirements"]
                         if row["native"].get("artifact_sha256") ==
                            single["capability_probe"]["artifact_sha256"]]
                self.assertTrue(cited)
                for row in cited:
                    self.assertEqual(1, len(row["native"]["run_ids"]), row["id"])
                    self.assertEqual([run["id"] for run in single["capability_probe"]["runs"]],
                                     row["native"]["run_ids"], row["id"])
                    self.assertEqual(single["capability_probe"]["artifact_sha256"],
                                     row["native"]["artifact_sha256"], row["id"])
                self.assertEqual(1, document["summary"]["satisfied"])
                self.assertEqual(61, document["summary"]["blocker"])
            finally:
                matrix.EVIDENCE = original

    def test_removing_one_evidence_axis_cannot_stay_green(self):
        document = matrix.generate()
        row = next(item for item in document["requirements"]
                   if item["verdict"] == "satisfied")
        for axis, replacement in (
            ("api", "blocker"), ("implementation", "missing"),
            ("cts", "mapped-not-run"), ("native", "not-run"),
        ):
            broken = copy.deepcopy(document)
            candidate = next(item for item in broken["requirements"]
                             if item["id"] == row["id"])
            candidate[axis]["state"] = replacement
            with self.assertRaisesRegex(ValueError, "non-fail-closed"):
                matrix.validate(broken)


if __name__ == "__main__":
    unittest.main()
