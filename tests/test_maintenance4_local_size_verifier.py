"""Reject altered, partial and misidentified LocalSizeId observations."""
import hashlib
import unittest

from tools.verify_maintenance4_local_size_witness import fixture_contract, verify


def fixture(local_x):
    contract = fixture_contract(local_x)
    log = (f"MAINTENANCE4_LOCAL_SIZE_START x={local_x} "
           f"specialization={int(local_x == 32)} groups=2\n"
           f"MAINTENANCE4_LOCAL_SIZE_RESULT x={local_x} outputs={2*local_x} "
           f"mismatches=0 guards=0 digest={contract['expected_digest']} fence=complete\n"
           "MAINTENANCE4_LOCAL_SIZE_RETIRED resources=clean\n").encode()
    receipt = {"protocol": "ps5log/1", "title": "PPSA99994", "app": "ps5vk",
               "transport": "tcp", "clean": True, "bye": True, "gaps": 0,
               "sha256": hashlib.sha256(log).hexdigest(), "run_id": "synthetic-test"}
    artifact = {**contract, **{key: "a" * 64 for key in
        ("eboot_sha256", "sdk_sha256", "source_sha256", "helper_sha256",
         "header_sha256", "spirv_sha256")}}
    return log, receipt, artifact


class Verify(unittest.TestCase):
    def test_exact_default_and_specialized_results(self):
        for local_x in (32, 64):
            with self.subTest(local_x=local_x):
                log, receipt, artifact = fixture(local_x)
                self.assertTrue(verify(log, receipt, artifact)["strict_verified"])

    def test_wrong_values_and_incomplete_transport(self):
        log, receipt, artifact = fixture(32)
        for before, after in ((b"outputs=64", b"outputs=128"),
                              (b"guards=0", b"guards=1"),
                              (b"fence=complete", b"fence=pending"),
                              (b"resources=clean", b"resources=retained"),
                              (b"specialization=1", b"specialization=0")):
            changed = log.replace(before, after)
            fresh = dict(receipt, sha256=hashlib.sha256(changed).hexdigest())
            with self.subTest(before=before), self.assertRaises(ValueError):
                verify(changed, fresh, artifact)
        for change in ({"bye": False}, {"gaps": 1}, {"clean": False},
                       {"sha256": "a" * 64}, {"transport": "udp"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                verify(log, dict(receipt, **change), artifact)
        with self.assertRaises(ValueError):
            verify(log, receipt, dict(artifact, specialization=False))
        with self.assertRaises(ValueError):
            verify(log, receipt, dict(artifact, spirv_sha256=""))


if __name__ == "__main__":
    unittest.main()
