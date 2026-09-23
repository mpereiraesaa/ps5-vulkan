"""Packaging checks only: never evidence that upstream cases executed."""
from pathlib import Path
import unittest
from tools.build_upstream_cts import tessellation_build_profile

ROOT = Path(__file__).resolve().parents[1]


class TessellationRegistrationTests(unittest.TestCase):
    def test_profile_records_experimental_switches_not_validation(self):
        normal = tessellation_build_profile({})
        self.assertFalse(normal["experimental"])
        self.assertEqual("0", normal["switches"]["PS5VK_BDA_DIAGNOSTIC"])
        self.assertEqual("1", tessellation_build_profile(
            {"PS5VK_BDA_DIAGNOSTIC": "1"})["switches"]["PS5VK_BDA_DIAGNOSTIC"])
        for name in normal["switches"]:
            with self.subTest(name=name):
                profile = tessellation_build_profile({name: "4"})
                self.assertTrue(profile["experimental"])
                self.assertEqual(profile["switches"][name], "4")
                self.assertEqual(profile["capability_validation"], "not-implied-by-build")
        self.assertEqual(normal, tessellation_build_profile({"UNRELATED_SECRET": "private"}))

    def test_original_factory_and_sources_are_wired(self):
        package = (ROOT / "cts/upstream/package_ps5.cpp").read_text()
        builder = (ROOT / "tools/build_upstream_cts.py").read_text()
        self.assertIn('new tcu::TestCaseGroup(m_testCtx, "tessellation")', package)
        self.assertIn('vkt::tessellation::createWindingTests(m_testCtx)', package)
        self.assertIn('vkt::tessellation::createCommonEdgeTests(m_testCtx)', package)
        self.assertIn('vkt::tessellation::createShaderInputOutputTests(m_testCtx)', package)
        self.assertIn('vkt::tessellation::createMiscDrawTests(m_testCtx)', package)
        self.assertIn('vkt::tessellation::createPrimitiveDiscardTests(m_testCtx)', package)
        self.assertIn('new tcu::TestCaseGroup(m_testCtx, "geometry_interaction")', package)
        self.assertIn('vkt::tessellation::createGeometryPassthroughTests(m_testCtx)', package)
        for name in ("vktTessellationWindingTests.cpp", "vktTessellationUtil.cpp",
                     "vktTessellationShaderInputOutputTests.cpp", "vktTessellationMiscDrawTests.cpp",
                     "vktTessellationPrimitiveDiscardTests.cpp",
                     "vktTessellationGeometryPassthroughTests.cpp",
                     "vktTessellationCommonEdgeTests.cpp"):
            self.assertIn('cts_root / "external/vulkancts/modules/vulkan/tessellation/' + name + '"', builder)
        self.assertIn('driver = ROOT / "dist-sdk/lib/libSceAgcDriver.so"', builder)


if __name__ == "__main__":
    unittest.main()
