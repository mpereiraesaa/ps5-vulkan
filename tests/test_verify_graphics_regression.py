"""Malformed execution evidence must fail each format verifier, not just pixels."""
import hashlib
import unittest

from tests import test_verify_sampled_formats as sample_fixture
from tests import test_verify_sampled_filtering as filter_fixture
from tests import test_verify_integer_sampled_formats as integer_fixture
from tools import verify_sampled_formats, verify_sampled_filtering, verify_integer_sampled_formats


class GraphicsRegressionVerifier(unittest.TestCase):
    def test_each_verifier_rejects_corrupt_compute_completion_and_retirement(self):
        pairs = (
            (sample_fixture.SampledFormatVerifier().fixture, verify_sampled_formats.validate),
            (filter_fixture.SampledFilteringVerifier().fixture, verify_sampled_filtering.validate),
            (integer_fixture.fixture, verify_integer_sampled_formats.validate),
        )
        mutations = (
            (b"outputs=0", b"outputs=1"),
            (b"guards=0", b"guards=1"),
            (b"checked=3072", b"checked=0"),
            (b"round=2", b"round=1"),
            (b"PS5VK_GRAPHICS_COMPLETED serial=1", b"PS5VK_GRAPHICS_COMPLETED serial=99"),
            (b"matching_event=1", b"matching_event=0"),
            (b"displayed=0", b"displayed=1"),
            (b"\tMARK\t", b"\tBOGUS\t"),
            (b"allocations_bytes=0", b"allocations_bytes=4096"),
        )
        for make_fixture, validate in pairs:
            for old, new in mutations:
                with self.subTest(verifier=validate.__module__, mutation=new):
                    log, metadata, artifact = make_fixture()
                    self.assertIn(old, log)
                    altered = log.replace(old, new, 1)
                    metadata["sha256"] = hashlib.sha256(altered).hexdigest()
                    with self.assertRaises(ValueError):
                        validate(altered, metadata, artifact)
