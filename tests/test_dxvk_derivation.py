import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "derive_dxvk_profile", ROOT / "tools/derive_dxvk_profile.py")
derive = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(derive)


class DxvkDerivationTests(unittest.TestCase):
    def test_exact_immutable_v262_pin_and_profile_shape(self):
        source, artifact = derive.source_pin()
        self.assertEqual("v2.6.2", source["tag"])
        self.assertEqual("9d6f54a1ade20d1d27dd421024717a636f3d8c68", source["commit"])
        self.assertEqual("dbb2c8c6a34ac52b62b9fcb318b85c47dca9d865c765d9538be3bde70cf341d3",
                         artifact["sha256"])
        document = json.loads(derive.OUTPUT.read_text())
        derive.validate_committed(document, source, artifact)
        self.assertEqual("1.3.204", document["profile"]["api_version"])
        self.assertEqual([
            "vulkan10requirements", "vulkan11requirements",
            "vulkan12requirements", "vulkan13requirements",
            "d3d11_baseline", "d3d11_level11_0",
        ], document["profile"]["capabilities"])
        self.assertEqual({
            "requirements": 62, "api-version": 1, "extension": 2,
            "feature": 49, "property": 10,
        }, document["summary"])

    def test_derivation_merges_duplicates_and_rejects_conflicts(self):
        upstream = {
            "profiles": {derive.PROFILE_ID: {
                "version": 1, "api-version": "1.3.204", "label": "x",
                "description": "x", "capabilities": ["a", "b"]}},
            "capabilities": {
                "a": {"features": {"VkPhysicalDeviceFeatures": {"x": True}}},
                "b": {"features": {"VkPhysicalDeviceFeatures": {"x": True}}},
            },
        }
        source = {"id": "dxvk", "tag": "v2.6.2", "tag_object": "a",
                  "commit": "b"}
        artifact = {"path": "VP_DXVK_requirements.json", "size_bytes": 1,
                    "sha256": "c", "git_blob_sha1": "d"}
        document = derive.derive(upstream, source, artifact)
        row = next(item for item in document["requirements"]
                   if item["kind"] == "feature")
        self.assertEqual(["a", "b"], row["capabilities"])
        upstream["capabilities"]["b"]["features"]["VkPhysicalDeviceFeatures"]["x"] = False
        with self.assertRaisesRegex(ValueError, "conflicting values"):
            derive.derive(upstream, source, artifact)

    def test_checked_header_is_exact_derivative(self):
        document = json.loads(derive.OUTPUT.read_text())
        self.assertEqual(derive.render_header(document), derive.HEADER.read_text())
        header = derive.HEADER.read_text()
        for row in document["requirements"]:
            if row["kind"] in ("feature", "property", "extension"):
                self.assertIn(row["name"], header)


if __name__ == "__main__":
    unittest.main()
