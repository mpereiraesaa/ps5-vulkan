"""The focused CTS package must register its original Broadcast factory."""
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CTS = ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/vulkan/subgroups"


class SubgroupFactory(unittest.TestCase):
    def test_original_factory_is_registered_but_not_selected(self):
        package = (ROOT / "cts/upstream/package_ps5.cpp").read_text()
        build = (ROOT / "tools/build_upstream_cts.py").read_text()
        manifest = json.loads((ROOT / "cts/upstream/manifest.json").read_text())
        self.assertIn("vkt::subgroups::createSubgroupsBallotBroadcastTests(m_testCtx)", package)
        self.assertIn('new tcu::TestCaseGroup(m_testCtx, "subgroups")', package)
        for name in ("vktSubgroupsBallotBroadcastTests.cpp", "vktSubgroupsTestsUtils.cpp"):
            self.assertIn(f'"external/vulkancts/modules/vulkan/subgroups/{name}"', build)
        if CTS.is_dir():
            factory = (CTS / "vktSubgroupsBallotBroadcastTests.cpp").read_text()
            self.assertIn('new TestCaseGroup(testCtx, "ballot_broadcast")', factory)
            self.assertIn("OPTYPE_BROADCAST_NONCONST", factory)
            self.assertIn("isSubgroupBroadcastDynamicIdSupported(context)", factory)
        self.assertFalse(any(".subgroups." in case["path"] for case in manifest["cases"]))


if __name__ == "__main__":
    unittest.main()
