"""Bounded Broadcast witness contract and strict receipt verification."""

import hashlib
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_t08_subgroup_broadcast_witness import expected_digest, verify


class SubgroupWitnessTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
