"""Pin the legal DeviceScope route and the original CTS eligibility gate."""
import json
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "third_party/vulkan-headers/registry/vk.xml"
CTS = (ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/vulkan"
       "/memory_model/vktMemoryModelMessagePassing.cpp")
ID = "feature:VkPhysicalDeviceVulkan12Features:vulkanMemoryModelDeviceScope"


class DeviceScopeCtsGate(unittest.TestCase):
    def test_public_khr_route_and_unmapped_original_cts(self):
        report = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        evidence = json.loads((ROOT / "conformance_inventory/dxvk_v262_evidence.json").read_text())
        matrix = json.loads((ROOT / "conformance_inventory/dxvk_v262_matrix.json").read_text())
        # The device reports Vulkan 1.3.0: the probe reads DeviceScope through
        # the core aggregate, and the KHR route stays public beside it.
        for profile in ("compute", "graphics"):
            self.assertEqual(report["profiles"][profile]["apiVersion"], 4206592)
            self.assertEqual(report["profiles"][profile]["memory_model_query"],
                             {"route": "VK_KHR_vulkan_memory_model",
                              "vulkanMemoryModel": True,
                              "vulkanMemoryModelDeviceScope": True})
            self.assertIs(report["profiles"][profile]["core_version_queries"][
                "VkPhysicalDeviceVulkan12Features"]["vulkanMemoryModelDeviceScope"], True)
        self.assertEqual(evidence["capability_probe"]["observed"][ID], 1)
        row = next(item for item in matrix["requirements"] if item["id"] == ID)
        self.assertEqual((row["api"]["state"], row["cts"]["state"], row["verdict"]),
                         ("satisfied", "not-mapped", "satisfied"))
        self.assertEqual(row["cts"]["cases"], [])

    def test_registry_khr_feature_route(self):
        if not REGISTRY.is_file():
            self.skipTest("pinned Vulkan registry unavailable")
        root = ET.parse(REGISTRY).getroot()
        extension = root.find(".//extensions/extension[@name='VK_KHR_vulkan_memory_model']")
        self.assertEqual(extension.get("depends"),
                         "VK_KHR_get_physical_device_properties2,VK_VERSION_1_1")
        feature = root.find(".//types/type[@name='VkPhysicalDeviceVulkanMemoryModelFeatures']")
        self.assertEqual(feature.get("structextends"),
                         "VkPhysicalDeviceFeatures2,VkDeviceCreateInfo")
        self.assertIn("vulkanMemoryModelDeviceScope",
                      {member.findtext("name") for member in feature.findall("member")})

    def test_original_behavioral_factory_requires_api_11_first(self):
        if not CTS.is_file():
            self.skipTest("pinned upstream CTS unavailable")
        source = CTS.read_text()
        support = source.split("void MemoryModelTestCase::checkSupport", 1)[1]
        support = support.split("void MemoryModelTestCase::", 1)[0]
        api_gate = support.index("contextSupports(vk::ApiVersion(0, 1, 1, 0))")
        feature_gate = support.index("getVulkanMemoryModelFeatures().vulkanMemoryModelDeviceScope")
        self.assertLess(api_gate, feature_gate)
        self.assertRegex(support, re.compile(r"if \(!m_data\.core11\).*?vulkanMemoryModelDeviceScope", re.S),
                         "The extension branch must query DeviceScope")
        factory = source.split("tcu::TestCaseGroup *createTests", 1)[1]
        self.assertIn('{0, "ext"}', factory)
        self.assertIn('{SCOPE_DEVICE, "device"}', factory)
        self.assertIn("(Scope)scopeCases[scopeNdx].value", factory)


if __name__ == "__main__":
    unittest.main()
