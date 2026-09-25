"""The reported device apiVersion is one switch, gated by the 1.1-1.3 contract."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "check_core_version_contract", ROOT / "tools/check_core_version_contract.py")
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)
REGISTRY = ROOT / "third_party/vulkan-headers/registry/vk.xml"


class CoreVersionContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.contract = json.loads((ROOT / "conformance_inventory/core_version_contract.json").read_text())
        cls.profile = (ROOT / "src/physical_device_profile.h").read_text()
        cls.internal = (ROOT / "src/vk_internal.h").read_text()
        cls.dispatch = (ROOT / "src/vk_dispatch.c").read_text()

    def run_check(self, contract=None, profile=None, internal=None, dispatch=None, assume=None):
        return checker.check(ROOT, assume=assume, contract=contract or self.contract,
                             profile_source=profile or self.profile,
                             internal_source=internal or self.internal,
                             dispatch_source=dispatch or self.dispatch, registry=False)

    @staticmethod
    def raised(profile, minor):
        return profile.replace("#define PS5VK_DEVICE_API_VERSION VK_API_VERSION_1_0",
                               f"#define PS5VK_DEVICE_API_VERSION VK_API_VERSION_1_{minor}")

    def test_shipping_tree_reports_the_backed_version(self):
        self.assertEqual((1, 0), checker.check(ROOT))

    def test_registry_surface_is_exact(self):
        commands = {version: {level: len(names) for level, names in data["commands"].items()}
                    for version, data in self.contract["versions"].items()}
        self.assertEqual({"global": 1, "instance": 11, "device": 16}, commands["1.1"])
        self.assertEqual({"global": 0, "instance": 0, "device": 13}, commands["1.2"])
        self.assertEqual({"global": 0, "instance": 1, "device": 36}, commands["1.3"])
        unconditional = {version: sorted(row["feature"] for row in data["mandatory_features"]
                                         if row["depends"] is None)
                         for version, data in self.contract["versions"].items()}
        self.assertEqual(["multiview"], unconditional["1.1"])
        self.assertIn("subgroupBroadcastDynamicId", unconditional["1.2"])
        self.assertIn("timelineSemaphore", unconditional["1.2"])
        self.assertEqual(16, len(unconditional["1.3"]))
        self.assertIn("synchronization2", unconditional["1.3"])
        self.assertEqual([23, 24, 23], [len(self.contract["versions"][v]["promoted_extensions"])
                                        for v in checker.VERSIONS])

    @unittest.skipUnless(REGISTRY.is_file(), "pinned registry checkout not present")
    def test_registry_drift_is_refused(self):
        contract = copy.deepcopy(self.contract)
        contract["versions"]["1.1"]["commands"]["device"].remove("vkTrimCommandPool")
        with self.assertRaisesRegex(AssertionError, "differs from the pinned registry"):
            checker.check(ROOT, contract=contract)

    def test_raising_the_switch_today_is_refused_with_every_gap(self):
        with self.assertRaises(AssertionError) as raised:
            self.run_check(profile=self.raised(self.profile, 1))
        message = str(raised.exception)
        self.assertIn("apiVersion 1.1 is not backed", message)
        self.assertIn("1.1 command vkGetDeviceQueue2 (device) does not resolve", message)
        self.assertIn("1.1 limit:maxPerSetDescriptors: blocker", message)
        self.assertIn("1.1 limit:subgroupSupportedOperations: missing", message)
        self.assertNotIn("1.2 ", message)

    def test_assume_version_reports_without_editing(self):
        with self.assertRaises(AssertionError) as raised:
            self.run_check(assume="1.3")
        message = str(raised.exception)
        self.assertIn("PS5VK_INSTANCE_API_VERSION 1.1 is lower", message)
        self.assertIn("1.2 command vkCmdDrawIndirectCount (device)", message)
        self.assertNotIn("vkWaitSemaphores", message)
        self.assertIn("1.3 feature:synchronization2: in-progress", message)
        self.assertIn("1.3 limit:maxInlineUniformBlockSize: in-progress", message)
        self.assertNotIn("limit:maxBufferSize", message)
        self.assertNotIn("limit:maxMemoryAllocationSize", message)

    def test_every_mandatory_feature_and_promoted_extension_needs_a_row(self):
        contract = copy.deepcopy(self.contract)
        rows = contract["versions"]["1.2"]["requirements"]
        contract["versions"]["1.2"]["requirements"] = [
            row for row in rows if row["id"] != "feature:hostQueryReset"]
        with self.assertRaisesRegex(AssertionError, "mandatory feature hostQueryReset"):
            self.run_check(contract=contract)
        contract = copy.deepcopy(self.contract)
        rows = contract["versions"]["1.3"]["requirements"]
        contract["versions"]["1.3"]["requirements"] = [
            row for row in rows if row["id"] != "extension:VK_EXT_private_data"]
        with self.assertRaisesRegex(AssertionError, "promoted extension VK_EXT_private_data"):
            self.run_check(contract=contract)

    def test_satisfied_needs_evidence_and_known_status(self):
        contract = copy.deepcopy(self.contract)
        row = contract["versions"]["1.1"]["requirements"][0]
        row.pop("evidence", None)
        row["status"] = "satisfied"
        with self.assertRaisesRegex(AssertionError, "without evidence"):
            self.run_check(contract=contract)
        contract = copy.deepcopy(self.contract)
        contract["versions"]["1.1"]["requirements"][0]["status"] = "probably"
        with self.assertRaisesRegex(AssertionError, "bad status"):
            self.run_check(contract=contract)

    def test_profile_must_report_and_validate_the_switch(self):
        profile = self.profile.replace("properties->apiVersion = PS5VK_DEVICE_API_VERSION;",
                                       "properties->apiVersion = VK_API_VERSION_1_3;")
        with self.assertRaisesRegex(AssertionError, "must report and validate"):
            self.run_check(profile=profile)

    def test_core_alias_resolves_only_through_an_implemented_entry(self):
        table = ('{"vkCmdDispatchBase", "vkCmdDispatchBaseKHR", VK_API_VERSION_1_1},\n'
                 '{"vkTrimCommandPool", "vkTrimCommandPoolKHR", VK_API_VERSION_1_1},\n')
        bare = self.dispatch.replace("ENTRY(vkCmdDispatchBaseKHR, DEVICE)", "")
        self.assertNotIn("vkCmdDispatchBase", checker.dispatch_surface(bare + table))
        surface = checker.dispatch_surface(self.dispatch + table)
        self.assertIn("vkCmdDispatchBase", surface)
        self.assertEqual("vkTrimCommandPoolKHR" in surface, "vkTrimCommandPool" in surface)

    def test_gate_opens_only_when_the_whole_version_is_met(self):
        contract = copy.deepcopy(self.contract)
        for row in contract["versions"]["1.1"]["requirements"]:
            row["status"], row["evidence"] = "satisfied", "synthetic"
        v11 = contract["versions"]["1.1"]["commands"]
        entries = "".join(f"    ENTRY({name}, DEVICE),\n" for level in checker.LEVELS
                          for name in v11[level])
        profile = self.raised(self.profile, 1)
        self.assertEqual((1, 1), self.run_check(contract=contract, profile=profile,
                                                dispatch=self.dispatch + entries))
        missing_one = entries.replace("    ENTRY(vkGetDeviceQueue2, DEVICE),\n", "")
        with self.assertRaisesRegex(AssertionError, r"\(1 unmet\)"):
            self.run_check(contract=contract, profile=profile, dispatch=self.dispatch + missing_one)
        internal = self.internal.replace("#define PS5VK_INSTANCE_API_VERSION VK_API_VERSION_1_1",
                                         "#define PS5VK_INSTANCE_API_VERSION VK_API_VERSION_1_0")
        with self.assertRaisesRegex(AssertionError, "INSTANCE_API_VERSION 1.0 is lower"):
            self.run_check(contract=contract, profile=profile, internal=internal,
                           dispatch=self.dispatch + entries)


if __name__ == "__main__":
    unittest.main()
