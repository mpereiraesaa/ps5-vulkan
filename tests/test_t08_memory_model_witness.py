"""Contracts for the isolated T08 public-SDK memory-model witness."""

import hashlib
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t08_memory_model_witness import checked_spirv  # noqa: E402
from run_t08_memory_model_witness import expected_digest, verify  # noqa: E402


def instruction(opcode, *operands):
    return [(len(operands) + 1) << 16 | opcode, *operands]


def extension(name):
    encoded = name.encode() + b"\0"
    encoded += b"\0" * (-len(encoded) % 4)
    return instruction(10, *struct.unpack(f"<{len(encoded) // 4}I", encoded))


def fixture_spirv(device=False):
    words = [0x07230203, 0x00010000, 0, 8, 0]
    for capability in (1, 5345, 5346) if device else (1, 5345):
        words += instruction(17, capability)
    words += extension("SPV_KHR_vulkan_memory_model")
    words += instruction(14, 0, 3)
    words += instruction(32, 1, 7, 2)
    words += instruction(225, 3, 4)
    return struct.pack(f"<{len(words)}I", *words)


def fixture_log(scope="device", *, mismatches=0, guard_mismatches=0,
                digest=None, retired=True):
    digest = expected_digest() if digest is None else digest
    lines = [
        f"T08_MEMORY_MODEL_WITNESS_START scope={scope} values=128 "
        "guards=16 negative_gate=pass",
        f"T08_MEMORY_MODEL_WITNESS_RESULT scope={scope} values=128 "
        f"mismatches={mismatches} guard_mismatches={guard_mismatches} "
        f"digest={digest:08x} fence=complete",
    ]
    if retired:
        lines.append(f"T08_MEMORY_MODEL_WITNESS_RETIRED scope={scope} "
                     "resources=clean")
    return ("\n".join(lines) + "\n").encode()


def fixture_receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk",
                transport="tcp", clean=True, bye=True, gaps=[],
                sha256=hashlib.sha256(log).hexdigest(), run_id="unit-run")


class SpirvContract(unittest.TestCase):
    def test_queue_and_device_scope(self):
        for scope in ("queue-family", "device"):
            with self.subTest(scope=scope):
                payload = fixture_spirv(scope == "device")
                checked = checked_spirv(payload, scope)
                self.assertIn(b"SPV_KHR_storage_buffer_storage_class\0", checked)
                self.assertEqual(checked_spirv(checked, scope), checked)

    def test_scope_mismatch_rejected(self):
        with self.assertRaises(ValueError):
            checked_spirv(fixture_spirv(), "device")
        with self.assertRaises(ValueError):
            checked_spirv(fixture_spirv(True), "queue-family")

    def test_corrupt_or_wrong_model_rejected(self):
        with self.assertRaises(ValueError):
            checked_spirv(fixture_spirv()[:-1], "queue-family")
        with self.assertRaises(ValueError):
            checked_spirv(fixture_spirv().replace(
                struct.pack("<III", *instruction(14, 0, 3)),
                struct.pack("<III", *instruction(14, 0, 2))), "queue-family")


class ReceiptContract(unittest.TestCase):
    def setUp(self):
        self.artifact = dict(scope="device", eboot_sha256="artifact-sha")

    def test_exact_complete_receipt(self):
        log = fixture_log()
        result = verify(log, fixture_receipt(log), self.artifact)
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["digest"], f"{expected_digest():08x}")

    def test_bad_data_or_guard_rejected(self):
        for changes in ({"mismatches": 1}, {"guard_mismatches": 1},
                        {"digest": 0}, {"retired": False},
                        {"scope": "queue-family"}):
            with self.subTest(changes=changes):
                log = fixture_log(**changes)
                with self.assertRaises(ValueError):
                    verify(log, fixture_receipt(log), self.artifact)

    def test_broken_transport_or_hash_rejected(self):
        log = fixture_log()
        for change in ({"clean": False}, {"bye": False}, {"gaps": [1]},
                       {"transport": "usb"}, {"sha256": "bad"}):
            with self.subTest(change=change):
                receipt = fixture_receipt(log) | change
                with self.assertRaises(ValueError):
                    verify(log, receipt, self.artifact)


if __name__ == "__main__":
    unittest.main()
