import copy
import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


derive = load_tool("derive_dxvk_profile")
matrix = load_tool("check_dxvk_profile")


class DxvkMatrixTests(unittest.TestCase):
    def test_matrix_is_exhaustive_and_fail_closed(self):
        document = matrix.generate()
        matrix.validate(document)
        profile = json.loads(derive.OUTPUT.read_text())
        self.assertEqual([row["id"] for row in profile["requirements"]],
                         [row["id"] for row in document["requirements"]])
        self.assertEqual(62, document["summary"]["requirements"])
        self.assertEqual(1, document["summary"]["satisfied"])
        self.assertEqual(61, document["summary"]["blocker"])
        self.assertEqual(
            ["feature:VkPhysicalDeviceFeatures:robustBufferAccess"],
            [row["id"] for row in document["requirements"]
             if row["verdict"] == "satisfied"])
        self.assertNotIn("not-run",
                         {row["native"]["state"] for row in document["requirements"]})

    def test_removing_one_evidence_axis_cannot_stay_green(self):
        document = matrix.generate()
        row = next(item for item in document["requirements"]
                   if item["verdict"] == "satisfied")
        for axis, replacement in (
            ("api", "blocker"), ("implementation", "missing"),
            ("cts", "mapped-not-run"), ("native", "not-run"),
        ):
            broken = copy.deepcopy(document)
            candidate = next(item for item in broken["requirements"]
                             if item["id"] == row["id"])
            candidate[axis]["state"] = replacement
            with self.assertRaisesRegex(ValueError, "non-fail-closed"):
                matrix.validate(broken)


if __name__ == "__main__":
    unittest.main()
