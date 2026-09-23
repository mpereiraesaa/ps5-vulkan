"""Contracts for the isolated T08 public-SDK buffer-address witness."""

import hashlib
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t08_bda_witness import checked_spirv  # noqa: E402
from run_t08_bda_witness import expected_digest, verify  # noqa: E402


def instruction(opcode, *operands):
    return [(len(operands) + 1) << 16 | opcode, *operands]


def fixture_spirv(*, physical=True, version=0x00010000):
    words = [0x07230203, version, 0, 8, 0]
    words += instruction(17, 1)
    if physical:
        words += instruction(17, 5347)
    encoded = b"SPV_KHR_physical_storage_buffer\0"
    encoded += b"\0" * (-len(encoded) % 4)
    words += instruction(10, *struct.unpack(f"<{len(encoded) // 4}I", encoded))
    words += instruction(14, 5348, 1)
    return struct.pack(f"<{len(words)}I", *words)


def fixture_log(*, mismatches=0, guard_mismatches=0, digest=None,
                offsets=(256, 256, 256), retired=True):
    digest = expected_digest() if digest is None else digest
    lines = [
        "T08_BDA_WITNESS_START values=64 guards=8 source_offset=4 query=khr",
        f"T08_BDA_WITNESS_RESULT values=64 source_offset=4 "
        f"bind_offsets={offsets[0]},{offsets[1]},{offsets[2]} "
        f"mismatches={mismatches} guard_mismatches={guard_mismatches} "
        f"digest={digest:08x} fence=complete",
    ]
    if retired:
        lines.append("T08_BDA_WITNESS_RETIRED resources=clean "
                     "gpu_addresses=distinct")
    return ("\n".join(lines) + "\n").encode()


def fixture_receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk",
                transport="tcp", clean=True, bye=True, gaps=[],
                sha256=hashlib.sha256(log).hexdigest(), run_id="unit-run")


class SpirvContract(unittest.TestCase):
    def test_exact_vulkan10_physical_address_module(self):
        checked_spirv(fixture_spirv())

    def test_missing_capability_or_wrong_version_rejected(self):
        for payload in (fixture_spirv(physical=False),
                        fixture_spirv(version=0x00010300),
                        fixture_spirv()[:-1]):
            with self.subTest(length=len(payload)):
                with self.assertRaises(ValueError):
                    checked_spirv(payload)


class ReceiptContract(unittest.TestCase):
    def setUp(self):
        self.artifact = dict(profile="t08-bda-public-sdk-witness",
                             values=64, guard_words_per_buffer=8,
                             source_offset_words=4,
                             eboot_sha256="artifact-sha")

    def test_exact_complete_receipt(self):
        log = fixture_log()
        result = verify(log, fixture_receipt(log), self.artifact)
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["digest"], f"{expected_digest():08x}")
        self.assertEqual(result["bind_offsets"], [256, 256, 256])

    def test_bad_data_guard_or_retirement_rejected(self):
        for changes in ({"mismatches": 1}, {"guard_mismatches": 1},
                        {"digest": 0}, {"retired": False},
                        {"offsets": (0, 256, 256)},
                        {"offsets": (256, 257, 256)}):
            with self.subTest(changes=changes):
                log = fixture_log(**changes)
                with self.assertRaises(ValueError):
                    verify(log, fixture_receipt(log), self.artifact)

    def test_duplicate_or_failure_marker_rejected(self):
        log = fixture_log()
        for payload in (log + log,
                        log + b"T08_BDA_WITNESS_FAILURE result=-1\n"):
            with self.assertRaises(ValueError):
                verify(payload, fixture_receipt(payload), self.artifact)

    def test_broken_transport_identity_or_artifact_rejected(self):
        log = fixture_log()
        for change in ({"clean": False}, {"bye": False}, {"gaps": [1]},
                       {"transport": "usb"}, {"sha256": "bad"}):
            with self.subTest(change=change):
                with self.assertRaises(ValueError):
                    verify(log, fixture_receipt(log) | change, self.artifact)
        with self.assertRaises(ValueError):
            verify(log, fixture_receipt(log), self.artifact | {"values": 128})


if __name__ == "__main__":
    unittest.main()
