"""Bounded Broadcast witness contract and strict receipt verification."""

import hashlib
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_t08_subgroup_broadcast_witness import expected_digest, verify
from build_upstream_cts import tessellation_build_profile


class SubgroupWitnessTests(unittest.TestCase):
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
            b"T08_SUBGROUP_START subgroups=4 outputs=128 ids=7,19,31,1 api=1.0\n"
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
            "public_profile": "vulkan-1.0-subgroup-disabled",
            "eboot_sha256": "synthetic-host",
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
            b"T08_SUBGROUP_IADD_START subgroups=4 outputs=128 ids=7,19,31,1 api=1.0\n"
            + ("T08_SUBGROUP_IADD_RESULT outputs=128 mismatches=0 guards=0 "
               f"digest={digest:08x} fence=complete\n").encode()
            + b"T08_SUBGROUP_IADD_RETIRED resources=clean\n"
        )
        receipt = dict(self.receipt, sha256=hashlib.sha256(log).hexdigest())
        artifact = dict(self.artifact,
                        profile="t08-subgroup-iadd-diagnostic-witness",
                        operation="iadd")
        result = verify(log, receipt, artifact)
        self.assertEqual(result["operation"], "iadd")
        self.assertEqual(result["digest"], f"{digest:08x}")
        wrong = log.replace(f"digest={digest:08x}".encode(), b"digest=00000000")
        with self.assertRaisesRegex(ValueError, "subgroup data"):
            verify(wrong, dict(receipt, sha256=hashlib.sha256(wrong).hexdigest()),
                   artifact)
        with self.assertRaisesRegex(ValueError, "subgroup data"):
            verify(log, receipt, dict(artifact,
                                     profile="t08-subgroup-broadcast-diagnostic-witness",
                                     operation="broadcast"))

    def test_int8_iadd_wraparound_contract(self):
        digest = expected_digest("iadd_int8")
        self.assertNotEqual(digest, expected_digest("iadd"))
        log = (
            b"T08_SUBGROUP_IADD_INT8_START subgroups=4 outputs=128 ids=7,19,31,1 api=1.0\n"
            + ("T08_SUBGROUP_IADD_INT8_RESULT outputs=128 mismatches=0 guards=0 "
               f"digest={digest:08x} fence=complete\n").encode()
            + b"T08_SUBGROUP_IADD_INT8_RETIRED resources=clean\n"
        )
        receipt = dict(self.receipt, sha256=hashlib.sha256(log).hexdigest())
        artifact = dict(self.artifact,
                        profile="t08-subgroup-iadd_int8-diagnostic-witness",
                        operation="iadd_int8")
        self.assertEqual(verify(log, receipt, artifact)["digest"], f"{digest:08x}")
        wrong = log.replace(f"digest={digest:08x}".encode(),
                            f"digest={expected_digest('iadd'):08x}".encode())
        with self.assertRaisesRegex(ValueError, "subgroup data"):
            verify(wrong, dict(receipt, sha256=hashlib.sha256(wrong).hexdigest()),
                   artifact)


if __name__ == "__main__":
    unittest.main()
