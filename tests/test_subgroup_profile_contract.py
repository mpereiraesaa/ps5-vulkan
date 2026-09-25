"""Keep the prepared subgroup routes behind the current public reporting gate."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "check_subgroup_profile_contract", ROOT / "tools/check_subgroup_profile_contract.py")
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class SubgroupProfileContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.contract = json.loads((ROOT / "conformance_inventory/subgroup_profile_contract.json").read_text())
        cls.report = json.loads((ROOT / "conformance_inventory/reporting_matrix.json").read_text())
        cls.matrix = json.loads((ROOT / "conformance_inventory/dxvk_v262_matrix.json").read_text())
        cls.profile_source = (ROOT / "src/physical_device_profile.h").read_text()
        cls.device_source = (ROOT / "src/vk_device.c").read_text()
        cls.dispatch_source = (ROOT / "src/vk_dispatch.c").read_text()

    def validate(self, contract=None, report=None, matrix=None, profile_source=None,
                 device_source=None, dispatch_source=None):
        checker.check_reporting(contract or self.contract, report or self.report,
                                matrix or self.matrix, profile_source or self.profile_source,
                                device_source or self.device_source,
                                dispatch_source or self.dispatch_source)

    def test_pinned_sources_and_public_gate(self):
        checker.check(ROOT)

    def test_new_api_version_requires_reaudit(self):
        report = copy.deepcopy(self.report)
        report["profiles"]["compute"]["apiVersion"] = 4202496
        with self.assertRaisesRegex(AssertionError, "API version"):
            self.validate(report=report)

    def test_core_11_command_alias_requires_reaudit(self):
        dispatch = self.dispatch_source.replace(
            "ENTRY(vkGetPhysicalDeviceFeatures2KHR, INSTANCE),",
            "ENTRY(vkGetPhysicalDeviceFeatures2KHR, INSTANCE),\n"
            "    ENTRY(vkGetPhysicalDeviceFeatures2, INSTANCE),")
        with self.assertRaisesRegex(AssertionError, "core command dispatch"):
            self.validate(dispatch_source=dispatch)

    def test_each_core_12_command_gap_requires_reaudit(self):
        commands = self.contract["api12_command_gate"]["core_commands"]
        self.assertEqual(7, len(commands))
        dispatched = set(self.contract["api12_command_gate"]["current_core_dispatch"])
        implemented = set(self.contract["api12_command_gate"]["current_implementations"])
        self.assertEqual({"vkResetQueryPool"}, dispatched)
        self.assertEqual({"vkResetQueryPool"}, implemented)
        # The Vulkan 1.1 instance-level implementations stay in place so the
        # 1.1 census passes and only the 1.2 command under test changes.
        base = "".join(f"VKAPI_ATTR void VKAPI_CALL {name}(void) {{}}\n" for name in
                       self.contract["api11_command_gate"]["current_implementations"])
        for command in commands:
            with self.subTest(command=command, surface="dispatch"):
                if command in dispatched:
                    dispatch = self.dispatch_source.replace(f"ENTRY({command}, DEVICE),", "")
                else:
                    dispatch = self.dispatch_source + f"\nENTRY({command}, DEVICE)\n"
                with self.assertRaisesRegex(AssertionError, "Vulkan 1.2 core command dispatch"):
                    self.validate(dispatch_source=dispatch)
            with self.subTest(command=command, surface="public"):
                prototype = f"VKAPI_ATTR void VKAPI_CALL {command}(void);"
                with self.assertRaisesRegex(AssertionError, "Vulkan 1.2 core public prototypes"):
                    checker.check_core_sources(self.contract, prototype, base)
            with self.subTest(command=command, surface="implementation"):
                implementation = base + ("" if command in implemented else
                                         f"VKAPI_ATTR void VKAPI_CALL {command}(void) {{}}")
                with self.assertRaisesRegex(AssertionError, "Vulkan 1.2 core implementations"):
                    checker.check_core_sources(self.contract, "", implementation)

    def test_subgroup_promotion_requires_reaudit(self):
        matrix = copy.deepcopy(self.matrix)
        row = next(row for row in matrix["requirements"] if
                   row["id"].endswith(":subgroupBroadcastDynamicId"))
        row["implementation"]["state"] = "implemented"
        with self.assertRaisesRegex(AssertionError, "matrix was promoted"):
            self.validate(matrix=matrix)

    def test_query_route_requires_reaudit(self):
        source = self.device_source + "\nVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES\n"
        with self.assertRaisesRegex(AssertionError, "query route"):
            self.validate(device_source=source)

    def test_explicit_zero_properties_query_is_compatible(self):
        source = """VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(
            VkPhysicalDevice p, VkPhysicalDeviceProperties2 *out)
        {
            for (VkBaseOutStructure *next = (VkBaseOutStructure *)out->pNext;
                 next; next = next->pNext) {
                if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES) {
                    VkPhysicalDeviceSubgroupProperties *properties =
                        (VkPhysicalDeviceSubgroupProperties *)next;
                    properties->subgroupSize = 0u;
                    properties->supportedStages = 0u;
                    properties->supportedOperations = 0u;
                    properties->quadOperationsInAllStages = VK_FALSE;
                }
            }
        }
        """
        self.validate(device_source=source)
        with self.assertRaisesRegex(AssertionError, "query route"):
            self.validate(device_source=source.replace("subgroupSize = 0u",
                                                       "subgroupSize = 32u"))


if __name__ == "__main__":
    unittest.main()
