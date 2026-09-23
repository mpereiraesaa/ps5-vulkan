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
    def test_shipping_memory_model_and_address_extensions(self):
        extensions = matrix.implemented_device_extensions()
        self.assertIn("VK_KHR_vulkan_memory_model", extensions)
        self.assertIn("VK_KHR_device_group", extensions)
        self.assertIn("VK_KHR_buffer_device_address", extensions)
        evidence = json.loads(matrix.EVIDENCE.read_text())
        self.assertEqual(evidence["capability_probe"]["device_extensions"],
                         len(extensions))
    def test_standard_ubo_khr_route_requires_reported_feature_and_extension(self):
        row = next(row for row in json.loads(derive.OUTPUT.read_text())["requirements"]
                   if row["id"] == matrix.STANDARD_UBO_ID)
        query = {"route": "VK_KHR_uniform_buffer_standard_layout",
                 "uniformBufferStandardLayout": True}
        reports = {"uniformBufferStandardLayout": {
            "kind": "extension-feature", "reported": True, "verdict": "satisfied"}}
        extensions = {"VK_KHR_uniform_buffer_standard_layout"}
        api, implementation = matrix.standard_ubo_axes(row, query, extensions, reports)
        self.assertEqual(("satisfied", "implemented"),
                         (api["state"], implementation["state"]))
        for bad in ({}, {**query, "route": "invented"},
                    {**query, "uniformBufferStandardLayout": "1"}):
            with self.assertRaises(ValueError):
                matrix.standard_ubo_axes(row, bad, extensions, reports)
        with self.assertRaises(ValueError):
            matrix.standard_ubo_axes(row, query, set(), reports)
        api, implementation = matrix.standard_ubo_axes(
            row, {**query, "uniformBufferStandardLayout": False}, extensions, reports)
        self.assertEqual(("blocker", "missing"),
                         (api["state"], implementation["state"]))
        api, implementation = matrix.standard_ubo_axes(row, query, extensions, {})
        self.assertEqual(("satisfied", "missing"),
                         (api["state"], implementation["state"]))

    def test_memory_model_khr_route_keeps_device_scope_independent(self):
        rows = {row["id"]: row for row in json.loads(derive.OUTPUT.read_text())["requirements"]}
        query = {"route": "VK_KHR_vulkan_memory_model", "vulkanMemoryModel": True,
                 "vulkanMemoryModelDeviceScope": False}
        reports = {"vulkanMemoryModel": {
            "kind": "extension-feature", "reported": True, "verdict": "satisfied"}}
        extensions = {"VK_KHR_vulkan_memory_model"}
        base = rows["feature:VkPhysicalDeviceVulkan12Features:vulkanMemoryModel"]
        scope = rows["feature:VkPhysicalDeviceVulkan12Features:vulkanMemoryModelDeviceScope"]
        api, implementation = matrix.memory_model_axes(base, query, extensions, reports)
        self.assertEqual(("satisfied", "implemented"),
                         (api["state"], implementation["state"]))
        api, implementation = matrix.memory_model_axes(scope, query, extensions, reports)
        self.assertEqual(("blocker", "missing"),
                         (api["state"], implementation["state"]))
        for bad in ({}, {**query, "route": "VK_VERSION_1_2"},
                    {**query, "vulkanMemoryModel": False,
                     "vulkanMemoryModelDeviceScope": True}):
            with self.assertRaises(ValueError):
                matrix.memory_model_axes(base, bad, extensions, reports)
        api, implementation = matrix.memory_model_axes(base, query, set(), reports)
        self.assertEqual(("blocker", "missing"),
                         (api["state"], implementation["state"]))

    def test_t08_rows_preserve_independent_evidence_axes(self):
        rows = {row["id"]: row for row in matrix.generate()["requirements"]}
        prefix = "feature:VkPhysicalDeviceVulkan12Features:"
        expected = {
            "bufferDeviceAddress": ("satisfied", "implemented", "cts-pass",
                                    "native-evidence", "satisfied"),
            "vulkanMemoryModel": ("satisfied", "implemented", "cts-pass",
                                  "native-evidence", "satisfied"),
            "vulkanMemoryModelDeviceScope": ("blocker", "implemented", "not-mapped",
                                             "native-evidence", "blocker"),
        }
        for feature, states in expected.items():
            row = rows[prefix + feature]
            self.assertEqual(states,
                tuple(row[axis]["state"] for axis in ("api", "implementation",
                       "cts", "native")) + (row["verdict"],))

    def test_bda_khr_route_requires_public_query_extensions_and_report(self):
        row = {r["id"]: r for r in matrix.generate()["requirements"]}[matrix.BDA_ID]
        query = {"route": "VK_KHR_buffer_device_address", "bufferDeviceAddress": True,
                 "bufferDeviceAddressCaptureReplay": False,
                 "bufferDeviceAddressMultiDevice": False}
        extensions = {"VK_KHR_buffer_device_address", "VK_KHR_device_group"}
        report = {"bufferDeviceAddress": {"kind": "extension-feature",
                  "reported": True, "verdict": "satisfied"}}
        api, implementation = matrix.buffer_address_axes(row, query, extensions, report)
        self.assertEqual(("satisfied", "implemented"),
                         (api["state"], implementation["state"]))
        self.assertEqual("VK_KHR_buffer_device_address", api["via"])
        for bad in ({**query, "route": "VK_VERSION_1_2"},
                    {**query, "bufferDeviceAddressCaptureReplay": True},
                    {**query, "bufferDeviceAddressMultiDevice": True}):
            with self.assertRaises(ValueError):
                matrix.buffer_address_axes(row, bad, extensions, report)
        for missing in (set(), {"VK_KHR_device_group"},
                        {"VK_KHR_buffer_device_address"}):
            api, implementation = matrix.buffer_address_axes(row, query, missing, report)
            self.assertEqual(("blocker", "missing"),
                             (api["state"], implementation["state"]))
        api, implementation = matrix.buffer_address_axes(row, query, extensions, {})
        self.assertEqual(("satisfied", "missing"),
                         (api["state"], implementation["state"]))

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
        # Three multiview rows, the three T03 draw rows, the clip/cull pair, the
        # independently witnessed fragment-storage and dual-source features, and
        # the four T05 rasterization and viewport features advance; API 1.3
        # remains a separate blocker.
        self.assertEqual(20, document["summary"]["satisfied"])
        self.assertEqual(42, document["summary"]["blocker"])

    def test_matrix_is_exhaustive_and_fail_closed(self):
        document = matrix.generate()
        matrix.validate(document)
        profile = json.loads(derive.OUTPUT.read_text())
        self.assertEqual([row["id"] for row in profile["requirements"]],
                         [row["id"] for row in document["requirements"]])
        self.assertEqual(62, document["summary"]["requirements"])
        self.assertEqual(20, document["summary"]["satisfied"])
        self.assertEqual(42, document["summary"]["blocker"])
        self.assertEqual(
            [
                         "feature:VkPhysicalDeviceFeatures:depthBiasClamp",
                         "feature:VkPhysicalDeviceFeatures:depthClamp",
                         "feature:VkPhysicalDeviceFeatures:drawIndirectFirstInstance",
                         "feature:VkPhysicalDeviceFeatures:dualSrcBlend",
                         "feature:VkPhysicalDeviceFeatures:fillModeNonSolid",
                         "feature:VkPhysicalDeviceFeatures:fragmentStoresAndAtomics",
                         "feature:VkPhysicalDeviceFeatures:fullDrawIndexUint32",
                         "feature:VkPhysicalDeviceFeatures:independentBlend",
                         "feature:VkPhysicalDeviceFeatures:multiDrawIndirect",
                         "feature:VkPhysicalDeviceFeatures:multiViewport",
                         "feature:VkPhysicalDeviceFeatures:robustBufferAccess",
                         "feature:VkPhysicalDeviceFeatures:sampleRateShading",
                         "feature:VkPhysicalDeviceFeatures:shaderClipDistance",
                         "feature:VkPhysicalDeviceFeatures:shaderCullDistance",
                         "feature:VkPhysicalDeviceVulkan11Features:multiview",
                         "feature:VkPhysicalDeviceVulkan12Features:bufferDeviceAddress",
                         "feature:VkPhysicalDeviceVulkan12Features:uniformBufferStandardLayout",
                         "feature:VkPhysicalDeviceVulkan12Features:vulkanMemoryModel",
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
                self.assertEqual(20, document["summary"]["satisfied"])
                self.assertEqual(42, document["summary"]["blocker"])
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
