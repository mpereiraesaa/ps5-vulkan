"""Contracts for the DXVK262-T10 public-SDK render witness verifier."""

import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_dxvk_render_witness import (  # noqa: E402
    EXTENT, SAMPLE_POINTS, expected_digest, expected_image, front_facing_clockwise, rgba,
    verify, expected_compute_digest, expected_inline_digest)

ARTIFACT = dict(profile="dxvk-render-public-sdk-witness", extent=64, format="R8G8B8A8_UNORM",
                diagnostic_switch=None, eboot_sha256="artifact-sha")


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
            verify(log, receipt(log), dict(ARTIFACT, diagnostic_switch="PS5VK_X"))
        with self.assertRaises(ValueError):
            verify(log, dict(receipt(log), bye=False), ARTIFACT)


class CacheVerify(unittest.TestCase):
    artifact = dict(ARTIFACT, profile="dxvk-cache-public-sdk-witness",
                    diagnostic_switch="PS5VK_PIPELINE_CACHE_CONTROL_DIAGNOSTIC", cache_execution_version=2)
    created = ("DXVK_CACHE_WITNESS_CREATED cold_misses=2 warm_derivatives=2 "
               "bases_retired=2 cache_retired=1\n")
    queries = "DXVK_CACHE_WITNESS_QUERIES normal=3072 discard=0\n"

    compute = ("DXVK_CACHE_WITNESS_COMPUTE words=1024 cold_misses=2 warm_derivatives=1 "
               "factor=5 mismatches=0 guards=0 inputs=0 digest=" + expected_compute_digest() +
               " submissions=1 fence=complete resources=retired\n")

    def log(self, created=None, queries=None, compute=None):
        return fixture_log().replace(b"DXVK_RENDER_WITNESS_STEP",
            ((self.compute if compute is None else compute) +
             (self.created if created is None else created) +
             (self.queries if queries is None else queries)).encode() + b"DXVK_RENDER_WITNESS_STEP")

    def test_cache_image_and_lifetime_result(self):
        log = self.log()
        out = verify(log, receipt(log), self.artifact)
        self.assertTrue(out["strict_verified"])
        self.assertEqual(out["profile"], self.artifact["profile"])

    def test_missing_duplicate_false_or_out_of_order_cache_evidence(self):
        for log in (self.log(created=""), self.log(queries=""),
                    self.log(created=self.created * 2), self.log(queries=self.queries * 2),
                    self.log(created=self.created.replace("bases_retired=2", "bases_retired=0")),
                    self.log(created=self.created.replace("cold_misses=2", "cold_misses=1")),
                    self.log(created=self.created.replace("warm_derivatives=2", "warm_derivatives=1")),
                    self.log(created=self.created.replace("cache_retired=1", "cache_retired=0")),
                    self.log(queries=self.queries.replace("3072", "0")),
                    self.log(queries=self.queries.replace("3072", str(2**64 - 1))),
                    self.log(queries=self.queries.replace("discard=0", "discard=1")),
                    self.log(created=self.queries, queries=self.created),
                    self.log() + b"DXVK_RENDER_WITNESS_PENDING resources=retained\n",
                    self.log().replace(b"full_mismatches=0", b"full_mismatches=1")):
            with self.subTest(log=log), self.assertRaises(ValueError):
                verify(log, receipt(log), self.artifact)

    def test_compute_result_requires_every_contract(self):
        variants = ["", self.compute * 2]
        for old, new in (("words=1024", "words=1023"), ("factor=5", "factor=3"),
                         ("cold_misses=2", "cold_misses=1"),
                         ("warm_derivatives=1", "warm_derivatives=0"),
                         ("mismatches=0", "mismatches=1"), ("guards=0", "guards=1"),
                         ("inputs=0", "inputs=1"), (expected_compute_digest(), "00000000"),
                         ("submissions=1", "submissions=0"), ("complete", "timeout"),
                         ("retired", "pending")):
            variants.append(self.compute.replace(old, new))
        for compute in variants:
            log = self.log(compute=compute)
            with self.subTest(compute=compute), self.assertRaises(ValueError):
                verify(log, receipt(log), self.artifact)
        log = self.log()
        with self.assertRaises(ValueError):
            verify(log, receipt(log), dict(self.artifact, cache_execution_version=1))

    def test_artifact_cannot_cross_profiles(self):
        for log, artifact in ((self.log(), ARTIFACT), (fixture_log(), self.artifact),
                              (self.log(), dict(self.artifact, diagnostic_switch=None))):
            with self.subTest(artifact=artifact), self.assertRaises(ValueError):
                verify(log, receipt(log), artifact)


class InlineVerify(unittest.TestCase):
    artifact = dict(ARTIFACT, profile="dxvk-inline-compute-public-sdk-witness",
                    diagnostic_switch="PS5VK_INLINE_UNIFORM_DIAGNOSTIC", inline_execution_version=1)
    compute = ("DXVK_INLINE_WITNESS_COMPUTE routes=4 words=1024 mismatches=0 guards=0 digest=" +
               expected_inline_digest() + " submissions=1 fence=complete resources=retired\n")

    def log(self, compute=None):
        return fixture_log().replace(b"DXVK_RENDER_WITNESS_STEP",
            (self.compute if compute is None else compute).encode() + b"DXVK_RENDER_WITNESS_STEP")

    def test_exact_inline_result(self):
        log = self.log()
        result = verify(log, receipt(log), self.artifact)
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["total_submissions"], 2)

    def test_inline_requires_each_result_and_ownership_field(self):
        variants = ["", self.compute * 2, self.compute + "DXVK_INLINE_WITNESS_COMPUTE malformed\n"]
        for old, new in (("routes=4", "routes=3"), ("words=1024", "words=1023"),
                         ("mismatches=0", "mismatches=1"), ("guards=0", "guards=1"),
                         (expected_inline_digest(), "00000000"), ("submissions=1", "submissions=0"),
                         ("complete", "timeout"), ("retired", "pending")):
            variants.append(self.compute.replace(old, new))
        for compute in variants:
            log = self.log(compute)
            with self.subTest(compute=compute), self.assertRaises(ValueError):
                verify(log, receipt(log), self.artifact)
        for log in (self.compute.encode() + fixture_log(), fixture_log() + self.compute.encode(),
                    self.log() + b"DXVK_RENDER_WITNESS_PENDING resources=retained\n"):
            with self.assertRaises(ValueError):
                verify(log, receipt(log), self.artifact)

    def test_inline_cannot_cross_profiles_or_versions(self):
        for log, artifact in ((self.log(), ARTIFACT), (fixture_log(), self.artifact),
                             (self.log(), dict(self.artifact, inline_execution_version=0)),
                             (self.log(), dict(self.artifact, diagnostic_switch=None)),
                             (CacheVerify().log(), self.artifact), (self.log(), CacheVerify.artifact)):
            with self.subTest(artifact=artifact), self.assertRaises(ValueError):
                verify(log, receipt(log), artifact)


if __name__ == "__main__":
    unittest.main()
