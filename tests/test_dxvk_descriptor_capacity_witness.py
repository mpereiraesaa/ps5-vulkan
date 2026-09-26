"""Host contracts for the descriptor-set capacity and dynamic-offset witnesses."""
import hashlib
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"tools/{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


build = load("build_dxvk_descriptor_capacity_witness")
run = load("run_dxvk_descriptor_capacity_witness")
SOURCE = (ROOT / "examples/dxvk_descriptor_capacity_witness/main.c").read_text()
RECEIPT = {"protocol": "ps5log/1", "title": "PPSA99994", "app": "ps5vk", "transport": "tcp",
           "clean": True, "bye": True, "gaps": 0, "run_id": "r"}


class DescriptorCapacityWitness(unittest.TestCase):
    def test_c_and_python_oracles_agree(self):
        self.assertIn("return (k & 255u) | (((k >> 8) | 0x40u) << 8) | (((k * 37u) & 255u) << 16) |",
                      SOURCE)
        self.assertIn("{ return (s ? 0xb0000000u : 0xa0000000u) | (i << 8) | (c << 4) | 0x5u; }", SOURCE)
        self.assertIn("enum { IMAGES = 1024, COMPUTE_IMAGES = IMAGES - 1, SIDE = 32, BATCH = 16 };", SOURCE)
        self.assertIn("enum { DRAWS = 4, STRIDE = 256, SLOTS = 2 * DRAWS };", SOURCE)
        self.assertEqual(build.texel(0), 0xff004000)
        self.assertEqual(len({build.texel(k) for k in range(build.IMAGES)}), build.IMAGES)

    def test_shaders_use_constant_indices_and_full_sets(self):
        compute = build.capacity_compute_source()
        fragment = build.capacity_fragment_source()
        self.assertIn("uniform texture2D images[1023];", compute)
        self.assertIn("uniform texture2D images[1024];", fragment)
        self.assertEqual(1023, len(re.findall(r"images\[\d+\]", compute)) - 1)
        self.assertEqual(1024, len(re.findall(r"case \d+u:", fragment)))
        glslang = shutil.which("glslangValidator")
        if not glslang:
            self.skipTest("glslangValidator required")
        with tempfile.TemporaryDirectory() as directory:
            payloads = build.compile_shaders(glslang, Path(directory))
            self.assertEqual(set(payloads), set(build.shader_sources()))

    def verify(self, variant, text):
        log = text.encode()
        receipt = dict(RECEIPT, sha256=hashlib.sha256(log).hexdigest())
        artifact = {"variant": variant, "profile": build.PROFILES[variant], "eboot_sha256": "e"}
        return run.verify(log, receipt, artifact)

    def test_capacity_verification(self):
        compute, pixels = run.expected_capacity()
        good = ("DESCRIPTOR_CAPACITY_WITNESS_START images=1024 compute_set=1024 pixel_set=1024\n"
                "DESCRIPTOR_CAPACITY_WITNESS_UPLOADED images=1024\n"
                f"DESCRIPTOR_CAPACITY_WITNESS_RESULT compute_mismatches=0 first_compute=-1"
                f" pixel_mismatches=0 first_pixel=-1 guard=cdcdcdcd digest_compute={compute:08x}"
                f" digest_pixels={pixels:08x}\nDESCRIPTOR_CAPACITY_WITNESS_RETIRED resources=clean\n")
        self.assertTrue(self.verify("capacity", good)["strict_verified"])
        with self.assertRaises(ValueError):
            self.verify("capacity", good.replace("pixel_mismatches=0", "pixel_mismatches=3"))

    def test_dynamic_verification(self):
        compute, pixels = run.expected_dynamic()
        good = ("DESCRIPTOR_DYNAMIC_WITNESS_START draws=4 stride=256\n"
                "DESCRIPTOR_DYNAMIC_WITNESS_STALE_RESUBMIT result=-13\n"
                f"DESCRIPTOR_DYNAMIC_WITNESS_RESULT compute_mismatches=0 pixel_mismatches=0"
                f" guards=0 stale_resubmit=-13 digest_compute={compute:08x}"
                f" digest_pixels={pixels}\nDESCRIPTOR_DYNAMIC_WITNESS_RETIRED resources=clean\n")
        self.assertTrue(self.verify("dynamic", good)["strict_verified"])
        with self.assertRaises(ValueError):
            self.verify("dynamic", good.replace("result=-13", "result=0").replace(
                "stale_resubmit=-13", "stale_resubmit=0"))


if __name__ == "__main__":
    unittest.main()
