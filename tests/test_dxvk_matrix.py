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
    def test_multiview_equivalence_never_invents_values_or_other_features(self):
        profile = json.loads(derive.OUTPUT.read_text())
        rows = [r for r in profile["requirements"] if r["id"] in matrix.MULTIVIEW_FIELDS]
        query = {"route": "VK_KHR_multiview", "multiview": True,
                 "maxMultiviewViewCount": 6, "maxMultiviewInstanceIndex": 134217727}
        extensions = {"VK_KHR_multiview"}
        for row in rows:
            field = matrix.MULTIVIEW_FIELDS[row["id"]]
            api, implementation = matrix.multiview_axes(row, query, extensions)
            self.assertEqual("satisfied", api["state"])
            self.assertEqual("implemented", implementation["state"])
            for bad in ({}, {**query, "route": "invented"}, {**query, field: "6"}):
                with self.assertRaises(ValueError):
                    matrix.multiview_axes(row, bad, extensions)
            with self.assertRaises(ValueError):
                matrix.multiview_axes(row, query, set())
            api, implementation = matrix.multiview_axes(
                row, {**query, field: False if field == "multiview" else 0}, extensions)
            self.assertEqual(("blocker", "missing"), (api["state"], implementation["state"]))
        self.assertIsNone(matrix.multiview_axes({"id": "api-version:apiVersion"}, query, extensions))

    def test_multiview_promotes_with_preserved_floor_evidence(self):
        """Promotion preserves the exact historical floor witnesses."""
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
            self.assertEqual("implemented", row["implementation"]["state"], identifier)
            self.assertEqual("cts-pass", row["cts"]["state"], identifier)
            self.assertEqual("VK_KHR_multiview", row["api"]["via"], identifier)
            self.assertEqual("satisfied", row["verdict"], identifier)
            self.assertEqual(48, len(row["cts"]["cases"]))
        # Both properties name exactly the run that measured their floor.
        self.assertNotEqual(
            rows["property:VkPhysicalDeviceVulkan11Properties:maxMultiviewViewCount"]["native"],
            rows["property:VkPhysicalDeviceVulkan11Properties:maxMultiviewInstanceIndex"]["native"])
        # Three multiview rows, the three T03 draw rows, the clip/cull pair and
        # the independently witnessed fragment-storage feature advance; API
        # 1.3 remains a separate blocker.
        self.assertEqual(10, document["summary"]["satisfied"])
        self.assertEqual(52, document["summary"]["blocker"])

    def test_matrix_is_exhaustive_and_fail_closed(self):
        document = matrix.generate()
        matrix.validate(document)
        profile = json.loads(derive.OUTPUT.read_text())
        self.assertEqual([row["id"] for row in profile["requirements"]],
                         [row["id"] for row in document["requirements"]])
        self.assertEqual(62, document["summary"]["requirements"])
        self.assertEqual(10, document["summary"]["satisfied"])
        self.assertEqual(52, document["summary"]["blocker"])
        self.assertEqual(
            [
                         "feature:VkPhysicalDeviceFeatures:drawIndirectFirstInstance",
                         "feature:VkPhysicalDeviceFeatures:fragmentStoresAndAtomics",
                         "feature:VkPhysicalDeviceFeatures:fullDrawIndexUint32",
                         "feature:VkPhysicalDeviceFeatures:multiDrawIndirect",
                         "feature:VkPhysicalDeviceFeatures:robustBufferAccess",
                         "feature:VkPhysicalDeviceFeatures:shaderClipDistance",
                         "feature:VkPhysicalDeviceFeatures:shaderCullDistance",
                         "feature:VkPhysicalDeviceVulkan11Features:multiview",
                         "property:VkPhysicalDeviceVulkan11Properties:maxMultiviewInstanceIndex",
                         "property:VkPhysicalDeviceVulkan11Properties:maxMultiviewViewCount"
            ],
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
                self.assertEqual(10, document["summary"]["satisfied"])
                self.assertEqual(52, document["summary"]["blocker"])
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
