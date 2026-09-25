"""Contracts for the DXVK262-T10 public-SDK render witness verifier."""

import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_dxvk_render_witness import (  # noqa: E402
    EXTENT, SAMPLE_POINTS, expected_digest, expected_image, front_facing_clockwise, rgba,
    verify)

ARTIFACT = dict(profile="dxvk-render-public-sdk-witness", extent=64, format="R8G8B8A8_UNORM",
                diagnostic_switch="PS5VK_DXVK_RENDER_DIAGNOSTIC", eboot_sha256="artifact-sha")


def fixture_log(*, features=(1, 1), mismatches=(0, 0, 0), visible=None, top=None,
                digest=None, samples=None, retired=True, failure=False):
    image, expected_visible, expected_top = expected_image()
    samples = samples or [image[y * EXTENT + x] for x, y in SAMPLE_POINTS]
    lines = ["DXVK_RENDER_WITNESS_START extent=64 dynamicRendering=%d extendedDynamicState=%d"
             % features,
             "DXVK_RENDER_WITNESS_STEP index=0 fence=complete",
             "DXVK_RENDER_WITNESS_SAMPLES p0_0=%08x p47_63=%08x p48_0=%08x p63_40=%08x "
             "p63_63=%08x" % tuple(samples),
             "DXVK_RENDER_WITNESS_RESULT extent=64 full_mismatches=%d sub_mismatches=%d "
             "sentinel_mismatches=%d visible_markers=%x marker_top=%d digest=%s "
             "submissions=1 fence=complete" % (
                 *mismatches, expected_visible if visible is None else visible,
                 expected_top if top is None else top,
                 expected_digest() if digest is None else digest)]
    if failure:
        lines.append("DXVK_RENDER_WITNESS_FAILURE result=-3 step=vkQueueSubmit")
    if retired:
        lines.append("DXVK_RENDER_WITNESS_RETIRED resources=clean")
    return ("\n".join(lines) + "\n").encode()


def receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk", transport="tcp",
                clean=True, bye=True, gaps=[], sha256=hashlib.sha256(log).hexdigest(),
                run_id="unit-run")


class ExpectedImage(unittest.TestCase):
    def test_flip_selects_the_blue_marker_at_the_top(self):
        image, visible, top = expected_image()
        self.assertEqual((visible, top), (1 << 2, 0))
        self.assertEqual(image[0 * EXTENT + 63], rgba(0, 0, 255, 255))
        self.assertEqual(image[63 * EXTENT + 63], rgba(255, 0, 0, 255))

    def test_the_flip_the_cull_and_the_front_face_are_each_observable(self):
        # Without the negative height the green marker survives instead.
        _, visible, _ = expected_image((0.0, 0.0, 64.0, 64.0))
        self.assertEqual(visible, 1 << 1)
        # The two markers are wound opposite ways, so exactly one is front.
        self.assertNotEqual(front_facing_clockwise(6), front_facing_clockwise(12))

    def test_gradient_is_exact_unorm(self):
        image, _, _ = expected_image()
        self.assertEqual(image[5 * EXTENT + 7], rgba(28, 20, 64, 255))
        self.assertEqual(image[63 * EXTENT + 47], rgba(188, 252, 64, 255))


class Verify(unittest.TestCase):
    def test_accepts_the_exact_result(self):
        log = fixture_log()
        out = verify(log, receipt(log), ARTIFACT)
        self.assertTrue(out["strict_verified"])
        self.assertEqual(out["digest"], expected_digest())

    def test_refuses_every_deviation(self):
        image, _, _ = expected_image()
        bad_samples = [image[y * EXTENT + x] for x, y in SAMPLE_POINTS]
        bad_samples[3] = rgba(0, 255, 0, 255)
        for log in (fixture_log(features=(0, 1)), fixture_log(mismatches=(1, 0, 0)),
                    fixture_log(mismatches=(0, 1, 0)), fixture_log(mismatches=(0, 0, 1)),
                    fixture_log(visible=6), fixture_log(top=32),
                    fixture_log(digest="00000000"), fixture_log(samples=bad_samples),
                    fixture_log(retired=False), fixture_log(failure=True)):
            with self.subTest(log=log):
                with self.assertRaises(ValueError):
                    verify(log, receipt(log), ARTIFACT)

    def test_refuses_a_foreign_artifact_or_receipt(self):
        log = fixture_log()
        with self.assertRaises(ValueError):
            verify(log, receipt(log), dict(ARTIFACT, diagnostic_switch=None))
        with self.assertRaises(ValueError):
            verify(log, dict(receipt(log), bye=False), ARTIFACT)


if __name__ == "__main__":
    unittest.main()
