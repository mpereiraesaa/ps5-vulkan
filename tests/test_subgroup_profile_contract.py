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

    def validate(self, contract=None, report=None, matrix=None, profile_source=None,
                 device_source=None):
        checker.check_reporting(contract or self.contract, report or self.report,
                                matrix or self.matrix, profile_source or self.profile_source,
                                device_source or self.device_source)

    def test_pinned_sources_and_public_gate(self):
        checker.check(ROOT)

    def test_new_api_version_requires_reaudit(self):
        report = copy.deepcopy(self.report)
        report["profiles"]["compute"]["apiVersion"] = 4202496
        with self.assertRaisesRegex(AssertionError, "API version"):
            self.validate(report=report)

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


if __name__ == "__main__":
    unittest.main()
