import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "third_party/vk-gl-cts"
MULTIVIEW_FAMILIES = (
    "dEQP-VK.multiview.renderpass2.clear_attachments.",
    "dEQP-VK.multiview.renderpass2.masks.",
    "dEQP-VK.multiview.renderpass2.index.",
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

    def test_multiview_deriver_matches_the_pinned_listing(self):
        """The gate derives the multiview leaves from the pinned module, and the
        derived paths are exactly the selection present in the pinned listing,
        family and query segments included."""
        source = (UPSTREAM / "external/vulkancts/modules/vulkan/multiview/"
                  "vktMultiViewRenderTests.cpp")
        if not source.is_file():
            self.skipTest("pinned vk-gl-cts checkout not present")
        text = source.read_text(encoding="utf-8", errors="replace")
        function = self.gate._source_function_at_line(text, 4908)
        derived = self.gate._multiview_leaf_paths(text, function)
        listing = (UPSTREAM / "external/vulkancts/mustpass/main/vk-default/multiview.txt")
        selected = {line.strip()
                    for line in listing.read_text().splitlines()
                    if line.strip().startswith(MULTIVIEW_FAMILIES)}
        self.assertTrue(selected)
        self.assertEqual(selected, derived)

    def test_selected_multiview_cases_are_declared_with_their_gate(self):
        cases = [c for c in self.manifest["cases"]
                 if c["path"].startswith(MULTIVIEW_FAMILIES)]
        self.assertEqual(96, len(cases))
        self.assertEqual(64, sum(1 for c in cases if ".index." in c["path"]))
        for case in cases:
            self.assertEqual("Pass", case["expected_status"])
            self.assertEqual(["VkPhysicalDeviceMultiviewFeatures::multiview"],
                             case["features_required"])
            self.assertIn("vktMultiViewRenderTests.cpp", case["source"])
            self.assertTrue(case["rationale"].strip())

    def test_duplicate_selection_fails_closed_without_the_checkout(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["cases"].append(copy.deepcopy(manifest["cases"][0]))
        failures = self.gate._duplicate_selection_failures(manifest)
        self.assertTrue(failures)
        self.assertIn("selected twice", failures[0])

    def test_unknown_and_untraceable_paths_fail_closed(self):
        """A path the pinned factory does not produce, and a path whose citation
        is absent, must both fail rather than pass silently."""
        if not UPSTREAM.is_dir():
            self.skipTest("pinned vk-gl-cts checkout not present")
        for path, source in (
            # A leaf the pinned module does not produce.
            ("dEQP-VK.multiview.renderpass2.masks.get_query_pool_results.invented.15",
             "external/vulkancts/modules/vulkan/multiview/vktMultiViewRenderTests.cpp:4908"),
            # A real leaf whose citation points at a line that is not the
            # factory, so the deriver cannot vouch for it.
            ("dEQP-VK.multiview.renderpass2.masks.get_query_pool_results.5_10_5_10",
             "external/vulkancts/modules/vulkan/multiview/vktMultiViewRenderTests.cpp:1"),
            # A real view-index family name and a real mask under the wrong
            # parent: "masks" is a shader family and "15" is a real view-mask
            # leaf, but the pinned factory hangs view-index cases under the
            # index group's four stage families only.
            ("dEQP-VK.multiview.renderpass2.index.masks.get_query_pool_results.15",
             "external/vulkancts/modules/vulkan/multiview/vktMultiViewRenderTests.cpp:4908"),
        ):
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
                    self.assertEqual(1, self.gate.main(),
                                     f"{path} must fail closed")
                finally:
                    self.gate.MANIFEST = original

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
