"""Host contracts for the compute subgroup BASIC diagnostic witness."""
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


run = load("run_t08_subgroup_basic_witness")
SHADER = ROOT / "experiments/compute/t08_subgroup_basic_runtime.comp"


class SubgroupBasicWitness(unittest.TestCase):
    def test_c_and_python_oracles_agree(self):
        source = (ROOT / "examples/t08_subgroup_basic_witness/main.c").read_text()
        self.assertIn("case 5: return (lane & 1) ? lane == 1 : 2;", source)
        self.assertIn("salts[GROUPS] = {UINT32_C(0x1000), UINT32_C(0x2000)}", source)
        words = run.expected_words()
        self.assertEqual(896, len(words))
        self.assertEqual([32, 0, 0, 2, 1, 2, 3 + 0x1000], words[:7])
        self.assertEqual([32, 1, 0, 2, 0, 1, 0x1000], words[7:14])
        self.assertEqual(1, words[(64 + 32) * 7 + 2])  # workgroup 1, second subgroup

    def verify_log(self, result_line):
        log = ("T08_SUBGROUP_BASIC_START groups=2 local=16x4 subgroups=4 fields=7 api=1.0\n"
               + result_line + "T08_SUBGROUP_BASIC_RETIRED resources=clean\n").encode()
        receipt = {"protocol": "ps5log/1", "title": "PPSA99994", "app": "ps5vk",
                   "transport": "tcp", "clean": True, "bye": True, "gaps": 0,
                   "sha256": hashlib.sha256(log).hexdigest(), "run_id": "r"}
        artifact = {"profile": "t08-subgroup-basic-diagnostic-witness", "groups": 2,
                    "fields": 7, "public_profile": "vulkan-1.0-subgroup-disabled",
                    "eboot_sha256": "e"}
        return run.verify(log, receipt, artifact)

    def test_strict_verification(self):
        good = (f"T08_SUBGROUP_BASIC_RESULT outputs=896 mismatches=0 guards=0 "
                f"digest={run.expected_digest():08x} fence=complete\n")
        self.assertTrue(self.verify_log(good)["strict_verified"])
        with self.assertRaises(ValueError):
            self.verify_log(good.replace("mismatches=0", "mismatches=1"))
        with self.assertRaises(ValueError):
            self.verify_log(good.replace(f"digest={run.expected_digest():08x}",
                                         "digest=00000000"))

    def test_shader_is_exactly_compute_basic(self):
        glslang = shutil.which("glslangValidator")
        if not glslang:
            self.skipTest("glslangValidator required")
        build = load("build_t08_subgroup_basic_witness")
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "basic.spv"
            subprocess.run([glslang, "-V", "--target-env", "vulkan1.1", str(SHADER),
                            "-o", str(out)], check=True, capture_output=True)
            build.checked_spirv(out.read_bytes())
            with self.assertRaises(ValueError):
                build.checked_spirv(b"\0" * 28)

    def test_diagnostic_switch_is_private_and_default_off(self):
        platform = (ROOT / "native/platform_ps5.c").read_text()
        self.assertIn("#if defined(PS5VK_SUBGROUP_BASIC_DIAGNOSTIC) && "
                      "PS5VK_SUBGROUP_BASIC_DIAGNOSTIC", platform)
        self.assertNotIn("SUBGROUP_BASIC", (ROOT / "src/vk_device.c").read_text())
        self.assertIn('"PS5VK_SUBGROUP_BASIC_DIAGNOSTIC"',
                      (ROOT / "tools/build_upstream_cts.py").read_text())


if __name__ == "__main__":
    unittest.main()
