"""Packaging checks for the original resource and query CTS factories."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UpstreamResourceQueryRegistrationTests(unittest.TestCase):
    def test_original_factories_are_registered_under_manifest_paths(self):
        package = (ROOT / "cts/upstream/package_ps5.cpp").read_text()
        self.assertIn('#include "vktQueryPoolTests.hpp"', package)
        self.assertIn('#include "vktShaderRenderTextureGatherTests.hpp"', package)
        self.assertIn('vkt::QueryPool::createTests(m_testCtx, "query_pool")', package)
        self.assertIn('new tcu::TestCaseGroup(m_testCtx, "shaderrender")', package)
        self.assertIn('vkt::sr::createTextureGatherTests(m_testCtx)', package)

    def test_builder_compiles_upstream_factory_and_body_sources(self):
        builder = (ROOT / "tools/build_upstream_cts.py").read_text()
        for name in (
            "vktQueryPoolTests.cpp",
            "vktQueryPoolOcclusionTests.cpp",
            "vktQueryPoolStatisticsTests.cpp",
            "vktQueryPoolConcurrentTests.cpp",
            "vktQueryPoolFragInvocationTests.cpp",
            "vktQueryPoolPerformanceTests.cpp",
            "vktShaderRender.cpp",
            "vktShaderRenderTextureGatherTests.cpp",
        ):
            with self.subTest(name=name):
                self.assertIn(name, builder)


if __name__ == "__main__":
    unittest.main()
