"""Bounded Broadcast witness contract and strict receipt verification."""

import hashlib
import shutil
import struct
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_t08_subgroup_broadcast_witness import expected_digest, verify
from build_upstream_cts import tessellation_build_profile
from build_t08_subgroup_broadcast_witness import checked_spirv, diagnostic_environment


def profile(operation):
    return tessellation_build_profile({
        "PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC": "1" if operation in ("broadcast", "ballot") else "0",
        "PS5VK_SUBGROUP_IADD_DIAGNOSTIC": "1" if operation in
        ("iadd", "iadd_int8", "iadd_int16", "iadd_int64", "fadd_float16") else "0",
        "PS5VK_SHADER_INT8_DIAGNOSTIC": "1" if operation == "iadd_int8" else "0",
        "PS5VK_SHADER_INT16_DIAGNOSTIC": "1" if operation == "iadd_int16" else "0"})


class SubgroupWitnessTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("glslangValidator"), "glslangValidator unavailable")
    def test_ballot_witness_retains_every_operation_and_runtime_source(self):
        with tempfile.TemporaryDirectory() as directory:
            shader = Path(directory) / "ballot.spv"
            subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.2",
                            str(ROOT / "experiments/compute/t08_subgroup_ballot_runtime.comp"),
                            "-o", str(shader)], check=True, capture_output=True)
            words = list(struct.unpack(f"<{shader.stat().st_size // 4}I", shader.read_bytes()))
        checked_spirv(struct.pack(f"<{len(words)}I", *words), "ballot")
        offset = 5
        while offset < len(words):
            size, opcode = words[offset] >> 16, words[offset] & 0xffff
            if opcode == 343:  # OpGroupNonUniformBallotFindLSB
                words[offset] = (size << 16) | 345  # Shuffle is not the ballot contract.
                break
            offset += size
        with self.assertRaisesRegex(ValueError, "BALLOT|ballot"):
            checked_spirv(struct.pack(f"<{len(words)}I", *words), "ballot")

    def test_builder_removes_unrelated_diagnostic_switches(self):
        environment = diagnostic_environment({"PS5VK_INLINE_UNIFORM_DIAGNOSTIC": "1",
            "PS5VK_SUBGROUP_IADD_DIAGNOSTIC": "1", "TASK_SENTINEL": "kept"},
            Path("/tmp/sdk"), "broadcast")
        self.assertNotIn("PS5VK_INLINE_UNIFORM_DIAGNOSTIC", environment)
        self.assertEqual(environment["PS5VK_SUBGROUP_IADD_DIAGNOSTIC"], "0")
        self.assertEqual(environment["PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC"], "1")
        self.assertEqual(environment["TASK_SENTINEL"], "kept")

    @unittest.skipUnless(shutil.which("glslangValidator"), "glslangValidator unavailable")
    def test_broadcast_witness_uses_spirv15_and_runtime_source_id(self):
        with tempfile.TemporaryDirectory() as directory:
            shader = Path(directory) / "broadcast.spv"
            subprocess.run([
                "glslangValidator", "-V", "--target-env", "vulkan1.2",
                str(ROOT / "experiments/compute/t08_subgroup_broadcast_runtime.comp"),
                "-o", str(shader),
            ], check=True, capture_output=True)
            words = list(struct.unpack(f"<{shader.stat().st_size // 4}I",
                                       shader.read_bytes()))
        checked_spirv(struct.pack(f"<{len(words)}I", *words))
        old_version = words[1]
        words[1] = 0x00010400
        with self.assertRaisesRegex(ValueError, "SPIR-V 1.5"):
            checked_spirv(struct.pack(f"<{len(words)}I", *words))
        words[1] = old_version
        constant_id = None
        broadcast_source_offset = None
        offset = 5
        while offset < len(words):
            size, opcode = words[offset] >> 16, words[offset] & 0xffff
            if opcode == 43 and constant_id is None:
                constant_id = words[offset + 2]
            if opcode == 337:
                broadcast_source_offset = offset + 5
            offset += size
        self.assertIsNotNone(constant_id)
        self.assertIsNotNone(broadcast_source_offset)
        source_id = words[broadcast_source_offset]
        words[broadcast_source_offset] = constant_id
        with self.assertRaisesRegex(ValueError, "storage-buffer-sourced ID"):
            checked_spirv(struct.pack(f"<{len(words)}I", *words))
        words[broadcast_source_offset] = source_id
        source_pointer = None
        store_value_offset = None
        offset = 5
        while offset < len(words):
            size, opcode = words[offset] >> 16, words[offset] & 0xffff
            if opcode == 61 and words[offset + 2] == source_id:
                source_pointer = words[offset + 3]
            offset += size
        self.assertIsNotNone(source_pointer)
        offset = 5
        while offset < len(words):
            size, opcode = words[offset] >> 16, words[offset] & 0xffff
            if opcode == 62 and words[offset + 1] == source_pointer:
                store_value_offset = offset + 2
            offset += size
        self.assertIsNotNone(store_value_offset)
        words[store_value_offset] = constant_id
        with self.assertRaisesRegex(ValueError, "storage-buffer-sourced ID"):
            checked_spirv(struct.pack(f"<{len(words)}I", *words))

    def test_diagnostic_switch_has_distinct_cts_build_identity(self):
        ordinary = tessellation_build_profile({})
        diagnostic = tessellation_build_profile({
            "PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC": "1"})
        self.assertEqual(ordinary["switches"]["PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC"],
                         "0")
        self.assertEqual(diagnostic["switches"]["PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC"],
                         "1")
        self.assertFalse(ordinary["experimental"])
        self.assertTrue(diagnostic["experimental"])
        iadd = tessellation_build_profile({"PS5VK_SUBGROUP_IADD_DIAGNOSTIC": "1"})
        self.assertEqual(ordinary["switches"]["PS5VK_SUBGROUP_IADD_DIAGNOSTIC"], "0")
        self.assertEqual(iadd["switches"]["PS5VK_SUBGROUP_IADD_DIAGNOSTIC"], "1")
        self.assertTrue(iadd["experimental"])

    def setUp(self):
        self.log = (
            b"T08_SUBGROUP_START subgroups=4 outputs=128 ids=7,19,31,1 api=1.3 public=off\n"
            + ("T08_SUBGROUP_RESULT outputs=128 mismatches=0 guards=0 "
               f"digest={expected_digest():08x} fence=complete\n").encode()
            + b"T08_SUBGROUP_RETIRED resources=clean\n"
        )
        self.receipt = {
            "protocol": "ps5log/1", "title": "PPSA99994", "app": "ps5vk",
            "transport": "tcp", "clean": True, "bye": True, "gaps": [],
            "run_id": "synthetic-host", "sha256": hashlib.sha256(self.log).hexdigest(),
        }
        self.artifact = {
            "profile": "t08-subgroup-broadcast-diagnostic-witness",
            "outputs": 128, "subgroups": 4, "source_lanes": [7, 19, 31, 1],
            "operation": "broadcast",
            "public_profile": "vulkan-1.3-compute-basic-only",
            "build_profile": profile("broadcast"),
            "eboot_sha256": "a" * 64, "shader_sha256": "b" * 64,
            "source_sha256": "c" * 64, "sdk_sha256": "d" * 64,
        }

    def test_exact_result(self):
        self.assertTrue(verify(self.log, self.receipt, self.artifact)["strict_verified"])

    def test_bad_output_rejected_even_with_valid_receipt_hash(self):
        log = self.log.replace(b"mismatches=0", b"mismatches=1")
        receipt = dict(self.receipt, sha256=hashlib.sha256(log).hexdigest())
        with self.assertRaisesRegex(ValueError, "subgroup data"):
            verify(log, receipt, self.artifact)

    def test_incomplete_receipt_rejected(self):
        with self.assertRaisesRegex(ValueError, "incomplete"):
            verify(self.log, dict(self.receipt, bye=False), self.artifact)

    def test_wrong_profile_rejected(self):
        with self.assertRaisesRegex(ValueError, "unexpected"):
            verify(self.log, self.receipt,
                   dict(self.artifact, public_profile="vulkan-1.2"))

    def test_iadd_exact_readback_contract(self):
        digest = expected_digest("iadd")
        log = (
            b"T08_SUBGROUP_IADD_START subgroups=4 outputs=128 ids=7,19,31,1 api=1.3 public=off\n"
            + ("T08_SUBGROUP_IADD_RESULT outputs=128 mismatches=0 guards=0 "
               f"digest={digest:08x} fence=complete\n").encode()
            + b"T08_SUBGROUP_IADD_RETIRED resources=clean\n"
        )
        receipt = dict(self.receipt, sha256=hashlib.sha256(log).hexdigest())
        artifact = dict(self.artifact,
                        profile="t08-subgroup-iadd-diagnostic-witness",
                        operation="iadd", build_profile=profile("iadd"))
        result = verify(log, receipt, artifact)
        self.assertEqual(result["operation"], "iadd")
        self.assertEqual(result["digest"], f"{digest:08x}")
        wrong = log.replace(f"digest={digest:08x}".encode(), b"digest=00000000")
        with self.assertRaisesRegex(ValueError, "subgroup data"):
            verify(wrong, dict(receipt, sha256=hashlib.sha256(wrong).hexdigest()),
                   artifact)
        with self.assertRaisesRegex(ValueError, "unexpected subgroup witness artifact"):
            verify(log, receipt, dict(artifact,
                                     profile="t08-subgroup-broadcast-diagnostic-witness",
                                     operation="broadcast"))

    def test_all_ballot_operations_exact_readback_contract(self):
        digest = expected_digest("ballot")
        log = (
            b"T08_SUBGROUP_BALLOT_START subgroups=4 outputs=128 ids=7,19,31,1 api=1.3 public=off\n"
            + ("T08_SUBGROUP_BALLOT_RESULT outputs=128 mismatches=0 guards=0 "
               f"digest={digest:08x} fence=complete\n").encode()
            + b"T08_SUBGROUP_BALLOT_RETIRED resources=clean\n"
        )
        receipt = dict(self.receipt, sha256=hashlib.sha256(log).hexdigest())
        artifact = dict(self.artifact,
                        profile="t08-subgroup-ballot-diagnostic-witness",
                        operation="ballot", build_profile=profile("ballot"))
        self.assertEqual(verify(log, receipt, artifact)["digest"], f"{digest:08x}")
        for bad in (log.replace(b"guards=0", b"guards=1"),
                    log.replace(b"fence=complete", b"fence=timeout"),
                    log.replace(f"digest={digest:08x}".encode(), b"digest=00000000"),
                    log + b"T08_SUBGROUP_BALLOT_RETIRED resources=clean\n"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                verify(bad, dict(receipt, sha256=hashlib.sha256(bad).hexdigest()), artifact)

    def test_int8_iadd_wraparound_contract(self):
        digest = expected_digest("iadd_int8")
        self.assertNotEqual(digest, expected_digest("iadd"))
        log = (
            b"T08_SUBGROUP_IADD_INT8_START subgroups=4 outputs=128 ids=7,19,31,1 api=1.3 public=off\n"
            + ("T08_SUBGROUP_IADD_INT8_RESULT outputs=128 mismatches=0 guards=0 "
               f"digest={digest:08x} fence=complete\n").encode()
            + b"T08_SUBGROUP_IADD_INT8_RETIRED resources=clean\n"
        )
        receipt = dict(self.receipt, sha256=hashlib.sha256(log).hexdigest())
        artifact = dict(self.artifact,
                        profile="t08-subgroup-iadd_int8-diagnostic-witness",
                        operation="iadd_int8", build_profile=profile("iadd_int8"))
        self.assertEqual(verify(log, receipt, artifact)["digest"], f"{digest:08x}")
        wrong = log.replace(f"digest={digest:08x}".encode(),
                            f"digest={expected_digest('iadd'):08x}".encode())
        with self.assertRaisesRegex(ValueError, "subgroup data"):
            verify(wrong, dict(receipt, sha256=hashlib.sha256(wrong).hexdigest()),
                   artifact)

    @unittest.skipUnless(shutil.which("glslangValidator"), "glslangValidator unavailable")
    def test_int16_iadd_signed_wraparound_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "int16.spv"
            subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.2",
                            str(ROOT / "experiments/compute/t08_subgroup_int16_iadd_runtime.comp"),
                            "-o", str(binary)], check=True, capture_output=True)
            payload = binary.read_bytes()
            checked_spirv(payload, "iadd_int16")
            with self.assertRaisesRegex(ValueError, "iadd"):
                checked_spirv(payload, "iadd")
            words = list(struct.unpack(f"<{len(payload) // 4}I", payload))
            offset = 5
            while offset < len(words):
                size, opcode = words[offset] >> 16, words[offset] & 0xffff
                if opcode == 21 and words[offset + 2] == 16 and words[offset + 3] == 1:
                    words[offset + 3] = 0
                    break
                offset += size
            else:
                self.fail("signed Int16 type missing")
            with self.assertRaisesRegex(ValueError, "iadd_int16"):
                checked_spirv(struct.pack(f"<{len(words)}I", *words), "iadd_int16")
        digest = expected_digest("iadd_int16")
        self.assertNotEqual(digest, expected_digest("iadd"))
        log = (
            b"T08_SUBGROUP_IADD_INT16_START subgroups=4 outputs=128 ids=7,19,31,1 api=1.3 public=off\n"
            + ("T08_SUBGROUP_IADD_INT16_RESULT outputs=128 mismatches=0 guards=0 "
               f"digest={digest:08x} fence=complete\n").encode()
            + b"T08_SUBGROUP_IADD_INT16_RETIRED resources=clean\n"
        )
        receipt = dict(self.receipt, sha256=hashlib.sha256(log).hexdigest())
        artifact = dict(self.artifact,
                        profile="t08-subgroup-iadd_int16-diagnostic-witness",
                        operation="iadd_int16", build_profile=profile("iadd_int16"))
        self.assertEqual(verify(log, receipt, artifact)["digest"], f"{digest:08x}")
        wrong = log.replace(f"digest={digest:08x}".encode(),
                            f"digest={expected_digest('iadd'):08x}".encode())
        with self.assertRaisesRegex(ValueError, "subgroup data"):
            verify(wrong, dict(receipt, sha256=hashlib.sha256(wrong).hexdigest()),
                   artifact)

    @unittest.skipUnless(shutil.which("glslangValidator"), "glslangValidator unavailable")
    def test_wide_type_arithmetic_contracts(self):
        for operation, shader_name, capability in (
                ("iadd_int64", "int64_iadd", 11),
                ("fadd_float16", "float16_fadd", 9)):
            with self.subTest(operation=operation), tempfile.TemporaryDirectory() as directory:
                binary = Path(directory) / "wide.spv"
                subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.2",
                                str(ROOT / f"experiments/compute/t08_subgroup_{shader_name}_runtime.comp"),
                                "-o", str(binary)], check=True, capture_output=True)
                payload = binary.read_bytes()
                checked_spirv(payload, operation)
                words = list(struct.unpack(f"<{len(payload) // 4}I", payload))
                offset = 5
                while offset < len(words):
                    size, opcode = words[offset] >> 16, words[offset] & 0xffff
                    if opcode == 17 and words[offset + 1] == capability:
                        words[offset + 1] = 1
                        break
                    offset += size
                else:
                    self.fail("wide shader capability missing")
                with self.assertRaisesRegex(ValueError, operation):
                    checked_spirv(struct.pack(f"<{len(words)}I", *words), operation)
                digest = expected_digest(operation)
                mark = {"iadd_int64": "T08_SUBGROUP_IADD_INT64",
                        "fadd_float16": "T08_SUBGROUP_FADD_FLOAT16"}[operation]
                log = (f"{mark}_START subgroups=4 outputs=128 ids=7,19,31,1 api=1.3 public=off\n"
                       f"{mark}_RESULT outputs=128 mismatches=0 guards=0 "
                       f"digest={digest:08x} fence=complete\n"
                       f"{mark}_RETIRED resources=clean\n").encode()
                receipt = dict(self.receipt, sha256=hashlib.sha256(log).hexdigest())
                artifact = dict(self.artifact,
                                profile=f"t08-subgroup-{operation}-diagnostic-witness",
                                operation=operation, build_profile=profile(operation))
                self.assertEqual(verify(log, receipt, artifact)["digest"], f"{digest:08x}")
                bad = log.replace(b"guards=0", b"guards=1")
                with self.assertRaisesRegex(ValueError, "subgroup data"):
                    verify(bad, dict(receipt, sha256=hashlib.sha256(bad).hexdigest()), artifact)


if __name__ == "__main__":
    unittest.main()
