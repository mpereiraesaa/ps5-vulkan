import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "third_party/vk-gl-cts"
MODULE = "external/vulkancts/modules/vulkan/multiview/vktMultiViewRenderTests.cpp"
# The legacy render-pass families this selection keeps. Their renderpass2
# counterparts need VK_KHR_create_renderpass2, which this device does not
# advertise.
MULTIVIEW_FAMILIES = (
    "dEQP-VK.multiview.clear_attachments.",
    "dEQP-VK.multiview.masks.",
    "dEQP-VK.multiview.index.vertex_shader.",
    "dEQP-VK.multiview.index.fragment_shader.",
)


def load_gate():
    spec = importlib.util.spec_from_file_location(
        "check_upstream_selection", ROOT / "tools/check_upstream_selection.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


class UpstreamSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gate = load_gate()
        cls.manifest = json.loads((ROOT / "cts/upstream/manifest.json").read_text())

    def setUp(self):
        self.source = UPSTREAM / MODULE
        if not self.source.is_file():
            self.skipTest("pinned vk-gl-cts checkout not present")
        text = self.source.read_text(encoding="utf-8", errors="replace")
        self.leaves = self.gate._multiview_leaf_requirements(
            text, self.gate._source_function_at_line(text, 4908))
        self.capabilities, self.capability_failures = self.gate._advertised_capabilities()

    def _runnable(self, path):
        leaf = self.leaves[path]
        return (not self.gate._unadvertised(leaf["required"], self.capabilities) and
                leaf["max_views"] <= self.capabilities["max_multiview_view_count"])

    def _gate_exit_code(self, path, source=f"{MODULE}:4908"):
        """Run the real gate over the selection with one offending acceptance
        entry swapped in, and return its exit code."""
        manifest = copy.deepcopy(self.manifest)
        manifest["cases"] = [case for case in manifest["cases"]
                             if not case["path"].startswith(MULTIVIEW_FAMILIES)]
        manifest["cases"].append({
            "path": path, "source": source, "category": "multiview-render-pass",
            "expected_status": "Pass",
            "features_required": ["VkPhysicalDeviceMultiviewFeatures::multiview"],
            "rationale": "deliberately wrong",
        })
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            manifest_path.write_text(json.dumps(manifest))
            original = self.gate.MANIFEST
            self.gate.MANIFEST = manifest_path
            try:
                return self.gate.main()
            finally:
                self.gate.MANIFEST = original

    def test_device_capabilities_come_from_the_device_sources(self):
        """The rule is only worth anything if the truth it reads is the device's
        own: five extensions without create_renderpass2, multiview reported true
        and both shader-multiview flags reported false."""
        self.assertEqual([], self.capability_failures)
        self.assertIn("VK_KHR_MULTIVIEW", self.capabilities["extensions"])
        self.assertNotIn("VK_KHR_CREATE_RENDERPASS_2", self.capabilities["extensions"])
        self.assertTrue(self.capabilities["features"]["multiview"])
        self.assertFalse(self.capabilities["features"]["multiviewGeometryShader"])
        self.assertFalse(self.capabilities["features"]["multiviewTessellationShader"])
        self.assertNotIn("geometryShader", self.capabilities["core_features"])
        self.assertEqual(6, self.capabilities["max_multiview_view_count"])

    def test_selection_is_exactly_the_runnable_legacy_subset(self):
        """The frozen selection is every legacy leaf of these four families the
        device can actually run - no more, and not one fewer - and each one
        exists verbatim in the pinned listing."""
        candidates = sorted(path for path in self.leaves if path.startswith(MULTIVIEW_FAMILIES))
        self.assertEqual(64, len(candidates))
        runnable = sorted(path for path in candidates if self._runnable(path))
        self.assertEqual(48, len(runnable))
        selected = sorted(case["path"] for case in self.manifest["cases"]
                          if case["path"].startswith(MULTIVIEW_FAMILIES))
        self.assertEqual(runnable, selected)
        listing = {line.strip() for line in
                   (UPSTREAM / "external/vulkancts/mustpass/main/vk-default/multiview.txt")
                   .read_text().splitlines() if line.strip()}
        for path in selected:
            self.assertIn(path, listing)
            self.assertIn(path, self.leaves)

    def test_rendering_type_and_stage_requirements_are_derived(self):
        """The prerequisites a leaf is held to follow the rendering type and the
        stage family the factory itself gates on, so legacy leaves carry no
        create_renderpass2 requirement and the excluded stages do."""
        legacy = self.leaves["dEQP-VK.multiview.masks.get_query_pool_results.15"]
        renderpass2 = self.leaves["dEQP-VK.multiview.renderpass2.masks.get_query_pool_results.15"]
        geometry = self.leaves["dEQP-VK.multiview.index.geometry_shader.get_query_pool_results.15"]
        tessellation = self.leaves[
            "dEQP-VK.multiview.index.tessellation_shader.get_query_pool_results.15"]
        self.assertNotIn("extension:VK_KHR_CREATE_RENDERPASS_2", legacy["required"])
        self.assertIn("extension:VK_KHR_CREATE_RENDERPASS_2", renderpass2["required"])
        self.assertIn("feature:multiviewGeometryShader", geometry["required"])
        self.assertIn("core:geometryShader", geometry["required"])
        self.assertIn("feature:multiviewTessellationShader", tessellation["required"])
        for leaf in (legacy, renderpass2, geometry, tessellation):
            self.assertIn("extension:VK_KHR_MULTIVIEW", leaf["required"])

    def test_acceptance_may_not_require_an_unadvertised_capability(self):
        """No acceptance leaf may silently require create_renderpass2, the
        geometry- or tessellation-shader multiview feature, or a view count the
        device does not report: each one becomes a failure at the gate."""
        for path in (
            # The renderpass2 counterpart of a selected leaf.
            "dEQP-VK.multiview.renderpass2.masks.get_query_pool_results.15",
            # Real families whose multiview shader features are reported false.
            "dEQP-VK.multiview.index.geometry_shader.get_query_pool_results.15",
            "dEQP-VK.multiview.index.tessellation_shader.get_query_pool_results.15",
            # A real leaf whose case renders more views than the reported floor.
            "dEQP-VK.multiview.masks.get_query_pool_results.8",
        ):
            self.assertIn(path, self.leaves, path)
            self.assertFalse(self._runnable(path), path)
            with self.subTest(path=path):
                self.assertEqual(1, self._gate_exit_code(path),
                                 f"{path} must fail closed as acceptance")

    def test_unproduced_and_miscited_paths_fail_closed(self):
        """A path the pinned factory does not produce, a path hung under a family
        that does not own it, and a path whose citation is not the factory must
        all fail rather than pass silently."""
        for path, source in (
            ("dEQP-VK.multiview.masks.get_query_pool_results.invented.15", f"{MODULE}:4908"),
            ("dEQP-VK.multiview.masks.get_query_pool_results.5_10_5_10", f"{MODULE}:1"),
            ("dEQP-VK.multiview.index.masks.get_query_pool_results.15", f"{MODULE}:4908"),
        ):
            with self.subTest(path=path):
                self.assertEqual(1, self._gate_exit_code(path, source),
                                 f"{path} must fail closed")

    def test_duplicate_selection_fails_closed_without_the_checkout(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["cases"].append(copy.deepcopy(manifest["cases"][0]))
        failures = self.gate._duplicate_selection_failures(manifest)
        self.assertTrue(failures)
        self.assertIn("selected twice", failures[0])

    def test_selection_must_match_the_compiled_cts_revision(self):
        """A selection is only meaningful against the revision the packaging
        build compiles, so a manifest that pins another commit fails closed."""
        if not UPSTREAM.is_dir() or self.gate._cts_revision_failures(self.manifest):
            self.skipTest("pinned vk-gl-cts checkout is not a comparable checkout")
        manifest = copy.deepcopy(self.manifest)
        manifest["cts_pin"]["commit"] = "0" * 40
        failures = self.gate._cts_revision_failures(manifest)
        self.assertEqual(1, len(failures))
        self.assertIn("selection pins", failures[0])
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            manifest_path.write_text(json.dumps(manifest))
            original = self.gate.MANIFEST
            self.gate.MANIFEST = manifest_path
            try:
                self.assertEqual(1, self.gate.main())
            finally:
                self.gate.MANIFEST = original

    def test_current_selection_passes(self):
        self.assertEqual(0, self.gate.main())


if __name__ == "__main__":
    unittest.main()
