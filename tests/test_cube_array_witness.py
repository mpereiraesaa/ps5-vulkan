import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from verify_cube_array_witness import validate  # noqa: E402


class CubeArrayWitnessTests(unittest.TestCase):
    def setUp(self):
        self.vertex_hash = "a" * 64
        self.fragment_hash = "b" * 64
        self.artifact = {
            "title": "PPSA99994",
            "profile": "image-cube-array-witness",
            "submit_enabled": True,
            "files": {"eboot.bin": "c" * 64},
            "cube_array": {
                "feature": "VkPhysicalDeviceFeatures.imageCubeArray",
                "cubes": 2,
                "faces_per_cube": 6,
                "layers": 12,
                "storage_layers": 12,
                "base_array_layer": 0,
                "face_extent": [4, 4],
                "target_extent": [192, 64],
                "format": "VK_FORMAT_R8G8B8A8_UNORM",
                "vertex_spirv_sha256": self.vertex_hash,
                "fragment_spirv_sha256": self.fragment_hash,
            },
        }
        self.messages = [
            "PS5VK_CONSUMER_CUBE_ARRAY_FEATURE imageCubeArray=1 enabled_by_features2=1",
            "PS5VK_CONSUMER_CUBE_ARRAY_START cubes=2 faces=6 layers=12 image=4x4 "
            "target=192x64 storage_layers=12 base_array_layer=0 vertex_sha256=" + self.vertex_hash +
            " fragment_sha256=" + self.fragment_hash,
            "PS5VK_CONSUMER_CUBE_ARRAY_RESULT cells=12 pixels=12288 mismatches=0 "
            "face_order=+x,-x,+y,-y,+z,-z cube_count=2",
            "PS5VK_CONSUMER_CUBE_ARRAY_RETIRED fence_complete=1 allocations=0",
            "PS5VK_CONSUMER_TEST_SUCCESS",
            "PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1",
            "PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1",
        ]
        log_lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=123"]
        log_lines.extend(f"{index}\t{index}\tMARK\t{message}"
                         for index, message in enumerate(self.messages, 1))
        log_lines.append(
            f"BYE seq={len(self.messages)} reason=consumer-cube-array-end")
        self.log = ("\n".join(log_lines) + "\n").encode()
        self.receipt = {
            "protocol": "ps5log/1",
            "transport": "tcp",
            "clean": True,
            "bye": True,
            "gaps": [],
            "raw_lines": 0,
            "sha256": hashlib.sha256(self.log).hexdigest(),
            "last_seq": len(self.messages),
            "identity": {"title": "PPSA99994", "app": "ps5vk", "boot": "123"},
        }

    def test_exact_two_cube_six_face_run_is_accepted(self):
        result = validate(self.log, self.receipt, self.artifact)
        self.assertEqual(result["pixels_checked"], 192 * 64)
        self.assertEqual(result["mismatches"], 0)
        self.assertEqual(result["cubes"], 2)

    def test_shifted_view_is_bound_to_artifact(self):
        self.artifact["cube_array"].update(storage_layers=13, base_array_layer=1)
        self.log = self.log.replace(b"storage_layers=12 base_array_layer=0",
                                    b"storage_layers=13 base_array_layer=1")
        self.receipt["sha256"] = hashlib.sha256(self.log).hexdigest()
        result = validate(self.log, self.receipt, self.artifact)
        self.assertEqual(result["base_array_layer"], 1)
        self.assertEqual(result["storage_layers"], 13)

    def test_shifted_artifact_cannot_accept_unshifted_run(self):
        self.artifact["cube_array"].update(storage_layers=13, base_array_layer=1)
        with self.assertRaisesRegex(ValueError, "witness identity and shape"):
            validate(self.log, self.receipt, self.artifact)

    def test_out_of_bounds_view_is_refused(self):
        self.artifact["cube_array"]["base_array_layer"] = 1
        with self.assertRaisesRegex(ValueError, "storage and view range"):
            validate(self.log, self.receipt, self.artifact)

    def test_nonzero_pixel_mismatch_is_refused(self):
        self.messages[2] = self.messages[2].replace("mismatches=0", "mismatches=1")
        log_lines = self.log.decode().splitlines()
        log_lines[3] = "3\t3\tMARK\t" + self.messages[2]
        self.log = ("\n".join(log_lines) + "\n").encode()
        self.receipt["sha256"] = hashlib.sha256(self.log).hexdigest()
        with self.assertRaisesRegex(ValueError, "all cube-array face/cube samples"):
            validate(self.log, self.receipt, self.artifact)

    def test_wrong_artifact_shader_hash_is_refused(self):
        self.artifact["cube_array"]["fragment_spirv_sha256"] = "d" * 64
        with self.assertRaisesRegex(ValueError, "witness identity and shape"):
            validate(self.log, self.receipt, self.artifact)


if __name__ == "__main__":
    unittest.main()
