"""Bounded registration of the pinned original buffer-address CTS body."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODULE = (ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/vulkan/"
          "binding_model/vktBindingBufferDeviceAddressTests.cpp")
spec = importlib.util.spec_from_file_location(
    "build_upstream_cts_bda_registration", ROOT / "tools/build_upstream_cts.py")
builder = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(builder)


class BdaCtsRegistration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not MODULE.is_file():
            raise unittest.SkipTest("pinned vk-gl-cts checkout not present")

    def test_two_original_compute_choices_preserve_body_and_oracle(self):
        original = MODULE.read_text()
        with tempfile.TemporaryDirectory() as directory:
            focused_path = Path(directory) / MODULE.name
            builder.write_focused_bda_source(MODULE, focused_path)
            focused = focused_path.read_text()
        marker = "    TestGroupCase setCases[] = {"
        self.assertEqual(original[:original.index(marker)],
                         focused[:focused.index(marker)])
        marker = "    TestGroupCase offsetCases[] = {"
        self.assertEqual(original[original.index(marker):],
                         focused[focused.index(marker):])
        choices = focused[focused.index("    TestGroupCase setCases[] = {"):
                          focused.index(marker)]
        for label in ("set0", "depth1", "basessbo", "load", "nostore",
                      "single", "std140", "comp"):
            self.assertEqual(1, choices.count(f'"{label}"'))
        self.assertIn('{OFFSET_ZERO, "offset_zero"}', focused)
        self.assertIn('{OFFSET_NONZERO, "offset_nonzero"}', focused)
        package = (ROOT / "cts/upstream/package_ps5.cpp").read_text()
        self.assertIn("createBufferDeviceAddressTests(m_testCtx)", package)

    def test_factory_choice_drift_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.cpp"
            focused = Path(directory) / "focused.cpp"
            source.write_text(MODULE.read_text().replace('"set0"', '"set00"', 1))
            with self.assertRaisesRegex(SystemExit, "case choices drift"):
                builder.write_focused_bda_source(source, focused)


if __name__ == "__main__":
    unittest.main()
