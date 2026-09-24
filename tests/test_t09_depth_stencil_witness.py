"""Contracts for the T09 public-SDK depth/stencil witness verifier."""

import hashlib
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t09_depth_stencil_witness import checked_spirv  # noqa: E402
from run_t09_depth_stencil_witness import (  # noqa: E402
    EXTENT, SAMPLE_POINTS, expected_depth, expected_stencil_digest,
    expected_stencil_plane, verify)


def depth_word(x, y, error=0.0):
    return struct.unpack("<I", struct.pack("<f", expected_depth(x, y) + error))[0]


def fixture_log(*, d32s8=0x4200, d24s8=0, mismatches=(0, 0, 0, 0), stencil=None,
                steps=6, depth_error=0.0, retired=True, stencil_samples=None):
    plane = expected_stencil_plane()
    stencil = expected_stencil_digest() if stencil is None else stencil
    samples = stencil_samples or [plane[y * EXTENT + x] for x, y in SAMPLE_POINTS]
    depths = [depth_word(x, y, depth_error) for x, y in SAMPLE_POINTS]
    lines = [f"T09_DS_WITNESS_START extent=64 d32s8={d32s8:08x} d24s8={d24s8:08x}"]
    lines += [f"T09_DS_WITNESS_STEP index={i} fence=complete" for i in range(steps)]
    lines.append("T09_DS_WITNESS_SAMPLES depth_0_0=%08x depth_63_0=%08x depth_0_63=%08x "
                 "depth_63_63=%08x stencil_0_0=%02x stencil_63_0=%02x stencil_0_63=%02x "
                 "stencil_63_63=%02x" % (*depths, *samples))
    lines.append("T09_DS_WITNESS_RESULT extent=64 depth_mismatches=%d stencil_mismatches=%d "
                 "depth_after_stencil_mismatches=%d stencil_after_depth_mismatches=%d "
                 "depth_digest=3cdde6c5 stencil_digest=%s submissions=%d fence=complete"
                 % (*mismatches, stencil, steps))
    if retired:
        lines.append("T09_DS_WITNESS_RETIRED resources=clean")
    return ("\n".join(lines) + "\n").encode()


def fixture_receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk", transport="tcp",
                clean=True, bye=True, gaps=[], sha256=hashlib.sha256(log).hexdigest(),
                run_id="unit-run")


ARTIFACT = dict(profile="t09-depth-stencil-public-sdk-witness", extent=64,
                format="D32_SFLOAT_S8_UINT", eboot_sha256="artifact-sha")


class ExpectedPattern(unittest.TestCase):
    def test_stencil_pattern_depends_on_every_coordinate_bit(self):
        plane = expected_stencil_plane()
        self.assertEqual(len(plane), EXTENT * EXTENT)
        self.assertEqual(plane[0], 0xa5)
        for bit in range(6):
            self.assertNotEqual(plane[0], plane[1 << bit])            # x bit
            self.assertNotEqual(plane[0], plane[(1 << bit) * EXTENT])  # y bit

    def test_depth_plane_is_distinct_per_pixel(self):
        values = {expected_depth(x, y) for y in range(EXTENT) for x in range(EXTENT)}
        self.assertEqual(len(values), EXTENT * EXTENT)


class ReceiptContract(unittest.TestCase):
    def test_exact_complete_receipt(self):
        log = fixture_log()
        result = verify(log, fixture_receipt(log), ARTIFACT)
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["stencil_digest"], expected_stencil_digest())
        self.assertEqual(result["d24s8_features"], "00000000")

    def test_small_depth_interpolation_error_is_tolerated(self):
        log = fixture_log(depth_error=1e-7)
        self.assertTrue(verify(log, fixture_receipt(log), ARTIFACT)["strict_verified"])

    def test_bad_data_steps_formats_or_retirement_rejected(self):
        for changes in ({"mismatches": (1, 0, 0, 0)}, {"mismatches": (0, 0, 0, 2)},
                        {"stencil": "00000000"}, {"steps": 5}, {"retired": False},
                        {"d32s8": 0x200}, {"d24s8": 0x200}, {"depth_error": 1e-3},
                        {"stencil_samples": [0x5a, 0, 0, 0]}):
            with self.subTest(changes=changes):
                log = fixture_log(**changes)
                with self.assertRaises(ValueError):
                    verify(log, fixture_receipt(log), ARTIFACT)

    def test_duplicate_or_failure_marker_rejected(self):
        log = fixture_log()
        for payload in (log + log, log + b"T09_DS_WITNESS_FAILURE result=-1 step=x completed=0\n"):
            with self.assertRaises(ValueError):
                verify(payload, fixture_receipt(payload), ARTIFACT)

    def test_broken_transport_or_artifact_rejected(self):
        log = fixture_log()
        for change in ({"clean": False}, {"bye": False}, {"gaps": [1]},
                       {"transport": "usb"}, {"sha256": "bad"}):
            with self.subTest(change=change):
                with self.assertRaises(ValueError):
                    verify(log, fixture_receipt(log) | change, ARTIFACT)
        with self.assertRaises(ValueError):
            verify(log, fixture_receipt(log), ARTIFACT | {"format": "D24_UNORM_S8_UINT"})


class SpirvContract(unittest.TestCase):
    def test_shader_capability_only(self):
        words = [0x07230203, 0x00010000, 0, 8, 0, (2 << 16) | 17, 1]
        checked_spirv(struct.pack(f"<{len(words)}I", *words))
        bad = words[:-1] + [5347]
        with self.assertRaises(ValueError):
            checked_spirv(struct.pack(f"<{len(bad)}I", *bad))


if __name__ == "__main__":
    unittest.main()
