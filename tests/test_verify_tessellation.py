"""Synthetic fault injection; no private runtime evidence in the repository."""
import sys
import hashlib
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from verify_tessellation import validate, CASES
from test_verify_geometry import Fixture


def fixture(variant=9):
    f = Fixture()
    name, vertices, expected = CASES[variant]
    f.artifact["tessellation_witness"] = {
        "variant": variant, "ring_mode": 4, "no_draw": 0, "build_id": "ab" * 8}
    additions = [
        f"PS5VK_TESS_RECEIPT build={'ab'*8} variant={name} vertices={vertices} no_draw=0 tessellation=1",
        "PS5VK_TESS_QUEUE_RING_BOUND serial=20 rc=0 state=1",
        "PS5VK_GRAPHICS_SUBMIT serial=20 rc=0",
        "PS5VK_TESS_QUEUE_RING_RESTORED serial=20 rc=0 state=0",
        "PS5VK_GRAPHICS_COMPLETED serial=20",
        f"PS5VK_TESS_CONTROL variant={name} rc=0 created=1 vertices={vertices} ink={expected} "
        f"expected={expected} covered={expected} missing=0 foreign=0 wrong_color=0 "
        "digest=123456789abcdef0 verified=1",
        "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
    ]
    f.rebuild(f.records[:-1] + additions + f.records[-1:])
    return f


class TessVerifierTests(unittest.TestCase):
    def test_positive_fixtures(self):
        for variant in CASES:
            f = fixture(variant)
            result = validate(f.log, f.receipt, f.artifact)
            self.assertTrue(result["tessellation_verified"])
            self.assertFalse(result["deployment_identity_verified"])
            self.assertFalse(result["process_exit_verified"])

    def test_missing_and_duplicate_records(self):
        for marker in ("TESS_RECEIPT", "TESS_QUEUE_RING_BOUND", "TESS_QUEUE_RING_RESTORED",
                       "GRAPHICS_COMPLETED", "TESS_CONTROL", "PLATFORM_CLOSE"):
            for duplicate in (False, True):
                with self.subTest(marker=marker, duplicate=duplicate):
                    f = fixture()
                    selected = next(r for r in f.records if f"PS5VK_{marker} " in r)
                    f.rebuild(f.records + [selected] if duplicate else
                              [r for r in f.records if r != selected])
                    with self.assertRaises(ValueError):
                        validate(f.log, f.receipt, f.artifact)

    def test_mutations_rejected(self):
        for old, new in (
            ("build=" + "ab"*8, "build=" + "cd"*8),
            ("wrong_color=0", "wrong_color=1"), ("covered=90", "covered=0"),
            ("expected=90", "expected=0"), ("no_draw=0", "no_draw=1"),
            ("created=1", "created=0"), ("vertices=3", "vertices=6"),
            ("state=0", "state=3"), ("allocations_bytes=0", "allocations_bytes=1"),
            ("RESTORED serial=20", "RESTORED serial=21"),
        ):
            with self.subTest(old=old):
                f = fixture()
                f.rebuild([r.replace(old, new) for r in f.records])
                with self.assertRaises(ValueError):
                    validate(f.log, f.receipt, f.artifact)

    def test_timeout_despite_cleanup_and_geometry_success(self):
        f = fixture()
        f.rebuild(f.records[:-1] + ["PS5VK_TESS_STALL wait=2"] + f.records[-1:])
        with self.assertRaisesRegex(ValueError, "timeout"):
            validate(f.log, f.receipt, f.artifact)

    def test_restore_after_readback_rejected(self):
        f = fixture()
        r = next(r for r in f.records if "RING_RESTORED" in r)
        records = [x for x in f.records if x != r]
        records.insert(-1, r)
        f.rebuild(records)
        with self.assertRaisesRegex(ValueError, "ordering"):
            validate(f.log, f.receipt, f.artifact)

    def test_missing_manifest_not_certified(self):
        f = fixture()
        del f.artifact["tessellation_witness"]
        with self.assertRaisesRegex(ValueError, "artifact profile"):
            validate(f.log, f.receipt, f.artifact)

    def test_stream_failures_even_with_matching_hash(self):
        for old, new in (
            (b"\tMARK\tPS5VK_PLATFORM_CLOSE", b"\tERR\tPS5VK_PLATFORM_CLOSE"),
            (b"\n2\t", b"\n1\t"),
            (b"reason=graphics-api-end", b"reason=graphics-api-failed"),
        ):
            with self.subTest(old=old):
                f = fixture()
                f.log = f.log.replace(old, new)
                f.receipt["sha256"] = hashlib.sha256(f.log).hexdigest()
                with self.assertRaises(ValueError):
                    validate(f.log, f.receipt, f.artifact)

    def test_corrupt_hash_and_unverified_profiles(self):
        f = fixture()
        f.receipt["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "log hash"):
            validate(f.log, f.receipt, f.artifact)
        for field, value in (("variant", 3), ("variant", 4), ("ring_mode", 3),
                             ("no_draw", 1), ("build_id", "unset")):
            f = fixture()
            f.artifact["tessellation_witness"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                validate(f.log, f.receipt, f.artifact)


if __name__ == "__main__":
    unittest.main()
