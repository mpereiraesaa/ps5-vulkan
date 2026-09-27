"""Offline artifact, oracle and strict-log contracts for subgroup occupancy."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


verify = load("verify_subgroup_size_witness")


class SubgroupSizeWitness(unittest.TestCase):
    def fixture(self, case):
        dims, flags, required = verify.CASES[case]
        artifact = {"profile": verify.PROFILE, "contract_version": 1, "case": case,
                    "dimensions": dims, "flags": flags, "required_size": required,
                    "groups": 2, "fields": 8, "eboot_sha256": "a"*64}
        log = (f"SUBGROUP_SIZE_START case={case} local={case} flags={flags} "
               f"required={int(required)} groups=2 fields=8\n"
               f"SUBGROUP_SIZE_RESULT outputs={len(verify.expected_words(case))} "
               f"mismatches=0 guards=0 digest={verify.expected_digest(case):08x} fence=complete\n"
               "SUBGROUP_SIZE_RETIRED resources=clean\n").encode()
        return log, self.receipt(log), artifact

    def receipt(self, log):
        return {"protocol": "ps5log/1", "title": "PPSA99994", "app": "ps5vk",
                "transport": "tcp", "clean": True, "bye": True, "gaps": 0,
                "sha256": hashlib.sha256(log).hexdigest(), "run_id": "host-synthetic"}

    def test_all_cases_and_partial_controls(self):
        for case in verify.CASES:
            with self.subTest(case=case):
                result = verify.verify(*self.fixture(case))
                self.assertTrue(result["strict_verified"])
                self.assertEqual(result["verification_scope"], "log_contents_only")
                self.assertIs(result["deployment_identity_verified"], False)
        partial = verify.expected_words("33x1x1")
        self.assertEqual([32, 0, 1, 2, 1, 1, 1, 32], partial[32*8:33*8])
        self.assertEqual([32, 0, 0, 1, 1, 1, 1, 0], verify.expected_words("1x1x1")[:8])

    def test_rejects_bad_or_extra_markers_and_identity(self):
        log, receipt, artifact = self.fixture("32x3x1")
        result_line = log.splitlines()[1]
        variants = [log.replace(b"mismatches=0", b"mismatches=1"),
                    log.replace(b"guards=0", b"guards=1"),
                    log.replace(b"fence=complete", b"fence=timeout"),
                    log.replace(b"fields=8", b"fields=7"),
                    log.replace(b"flags=3", b"flags=0"),
                    log.replace(b"required=0", b"required=1"),
                    log.replace(b"outputs=1536", b"outputs=1535"),
                    log.replace(b"digest=", b"digest=0"),
                    log+result_line+b"\n", log+b"SUBGROUP_SIZE_RESULT malformed\n",
                    log+b"SUBGROUP_SIZE_PENDING resources=retained\n",
                    log+b"SUBGROUP_SIZE_FAILURE result=-1\n",
                    b"\n".join(reversed(log.splitlines())),
                    b"\n".join(log.splitlines()[:2])]
        for bad in variants:
            with self.subTest(log=bad):
                with self.assertRaises(ValueError):
                    verify.verify(bad, self.receipt(bad), artifact)
        for key, value in (("sha256", "b"*64), ("clean", False), ("bye", False),
                           ("gaps", 1), ("transport", "file")):
            with self.assertRaises(ValueError):
                verify.verify(log, dict(receipt, **{key: value}), artifact)
        for key, value in (("profile", "old"), ("contract_version", 2),
                           ("case", "64x2x1"), ("flags", 0), ("required_size", True),
                           ("dimensions", [32, 1, 3]), ("eboot_sha256", "missing")):
            bad = copy.deepcopy(artifact); bad[key] = value
            with self.assertRaises(ValueError):
                verify.verify(log, receipt, bad)

    def test_public_c_oracle_matches_independent_python(self):
        # Compile only public Vulkan declarations. Garbage collection removes
        # the unused execution helper so no private runtime symbols are linked.
        source = '''#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "examples/dxvk_render_witness/subgroup_compute.h"
int main(void) {
 (void)subgroup_compute_witness;
 const unsigned totals[]={96,128,128,1024,33,1};
 for(unsigned n=0;n<6;++n) {
  uint32_t digest=2166136261u;
  for(unsigned i=0;i<totals[n]*16;++i)
   digest=(digest^subgroup_expected_word(totals[n],i/8,i%8))*16777619u;
  printf("%08x\\n",digest);
 }
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "oracle.c").write_text(source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-I"+str(ROOT), "-I"+str(ROOT / "third_party/vulkan-headers/include"),
                            str(path / "oracle.c"), "-o", str(path / "oracle")], check=True,
                           capture_output=True, text=True)
            actual = subprocess.check_output([str(path / "oracle")], text=True).splitlines()
        self.assertEqual(actual, [f"{verify.expected_digest(case):08x}" for case in verify.CASES])

    def test_builder_removes_overrides_instead_of_zeroing_capacity(self):
        build = load("build_subgroup_size_witness")
        inherited = {"PATH": "/bin", "PS5VK_TESS_OFFCHIP_CAPACITY_WG": "144",
                     "PS5VK_INLINE_UNIFORM_DIAGNOSTIC": "1", "PS5VK_TESS_GE_CNTL": "99"}
        env = build.diagnostic_environment(inherited, Path("/toolchain"))
        self.assertEqual(env, {"PATH": "/bin", "PS5_PAYLOAD_SDK": "/toolchain",
                              "PS5VK_USE_SDK": "1", "PS5VK_SUBGROUP_SIZE_DIAGNOSTIC": "1"})
        self.assertEqual(inherited["PS5VK_TESS_OFFCHIP_CAPACITY_WG"], "144")

    def test_cli_verifies_saved_evidence_without_console(self):
        log, receipt, artifact = self.fixture("1x1x1")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "run.log").write_bytes(log)
            (path / "receipt.json").write_text(json.dumps(receipt))
            (path / "artifact.json").write_text(json.dumps(artifact))
            subprocess.run(["python3", str(ROOT / "tools/verify_subgroup_size_witness.py"),
                            "--log", str(path / "run.log"), "--receipt", str(path / "receipt.json"),
                            "--artifact", str(path / "artifact.json"), "--out", str(path / "out.json")],
                           check=True, capture_output=True, text=True)
            self.assertTrue(json.loads((path / "out.json").read_text())["strict_verified"])


if __name__ == "__main__":
    unittest.main()
