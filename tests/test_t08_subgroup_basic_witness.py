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

    def test_pinned_compiler_does_not_read_tg_size(self):
        """The PS5 compute dispatch leaves TG_SIZE unset, so LocalInvocationIndex,
        SubgroupID and NumSubgroups must not be lowered from a scalar argument
        (the first native run read garbage wave IDs from it)."""
        glslang = shutil.which("glslangValidator")
        archive = ROOT / "build/libpsbc.host.a"
        if not glslang or not archive.is_file():
            self.skipTest("host PSBC archive and glslangValidator required")
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            spirv, probe = temp / "basic.spv", temp / "probe"
            subprocess.run([glslang, "-V", "--target-env", "vulkan1.1", str(SHADER),
                            "-o", str(spirv)], check=True, capture_output=True)
            subprocess.run(["cc", "-std=c11", "-Ithird_party/psbc-reference/libpsbc",
                            "tests/t08_compile_probe.c", str(archive), "-lstdc++", "-lm",
                            "-lpthread", "-o", str(probe)], cwd=ROOT, check=True,
                           capture_output=True)
            result = subprocess.run([str(probe), "subgroup", str(spirv), "none"],
                                    env={"PSBC_DEBUG_NIR": "1"}, capture_output=True,
                                    text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("result=0", result.stdout)
            final = result.stderr.split("shader: MESA_SHADER_COMPUTE")[-1]
            for name in ("load_local_invocation_index", "load_num_subgroups",
                         "load_subgroup_id"):
                self.assertNotIn(name, final)
            # The index math must come from the local invocation ID VGPRs.
            # The TG_SIZE lowering reads no vector argument at all (wave ID
            # from a scalar argument plus mbcnt), which is what the first
            # native run executed.
            self.assertIn("load_vector_arg_amd", final)

    def test_basic_is_shipping_with_its_exact_report(self):
        platform = (ROOT / "native/platform_ps5.c").read_text()
        self.assertIn("platform->supported_features_t09 |= "
                      "PS5VK_T09_FEATURE_SUBGROUP_BASIC_COMPUTE;", platform)
        device = (ROOT / "src/vk_device.c").read_text()
        self.assertIn("basic ? VK_SUBGROUP_FEATURE_BASIC_BIT : 0u", device)
        self.assertIn("basic ? VK_SHADER_STAGE_COMPUTE_BIT : 0u", device)


if __name__ == "__main__":
    unittest.main()
