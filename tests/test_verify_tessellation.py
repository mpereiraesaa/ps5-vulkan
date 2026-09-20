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
    if variant in (11, 12):
        additions[0] += " ls_hs_config=00080301@2d6"
    if variant in (26,28):
        additions[0] += " ls_hs_config=0007c301@2d6"
    if variant in (14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28):
        f.artifact["tessellation_witness"]["shared_pipelines"] = 2
        additions[1:1] = [
            "PS5VK_TESS_SHARED_STORAGE pipelines=2 same_storage=1 split_scissors=1",
            "PS5VK_GRAPHICS_PREPARED serial=20 draws=" + ("6" if variant in (17,19,20,21) else "2")]
    if variant in (17,19,20,21):
        dst="ZERO" if variant in (20,21) else "ONE"
        source="CONSTANT_ALPHA" if variant==21 else "SRC_ALPHA"
        additions.insert(1, f"PS5VK_TESS_BLEND draws=3 alpha=0.25 src={source} dst={dst} op=ADD")
    if variant == 15:
        additions.insert(1, "PS5VK_TESS_CACHE_REUSE hits_delta=1 compiles_delta=0 original_destroyed=1")
    if variant == 16:
        additions.insert(1, "PS5VK_TESS_GEOMETRY stages=5 shift_x=4 shift_y=-4 color=gbr")
    if variant == 18:
        additions.insert(-1, "PS5VK_TESS_ALPHA expected=64 tolerance=3 samples=2916 wrong=0 min=64 max=64")
    if variant == 22:
        additions.insert(1, "PS5VK_TESS_INDEXED type=uint16 binding_offset=2 first_index=1 vertex_offset=1 indices=4,1,6")
    if variant == 23:
        additions.insert(1, "PS5VK_TESS_INDEXED type=uint32 binding_offset=4 first_index=1 vertex_offset=-65536 indices=65541,65538,65543")
    if variant == 24:
        additions.insert(1, "PS5VK_TESS_INSTANCE count=2 first=3 expected_ids=3,4")
    if variant == 25:
        additions.insert(1, "PS5VK_TESS_INDIRECT offset=16 count=1 vertices=3 instances=2 first_vertex=0 first_instance=3 resolver=host_submit")
    if variant in (54,55):
        additions.insert(1, f"PS5VK_TESS_INDEXED_INSTANCE indirect={int(variant==55)} binding_offset=4 first_index=1 vertex_offset=-65536 instances=2 first_instance=3")
    if variant == 56:
        additions.insert(1, "PS5VK_TESS_PUSH_MEMBER vertex=0:16 control=16:68 selected=1")
    f.rebuild(f.records[:-1] + additions + f.records[-1:])
    return f


class TessVerifierTests(unittest.TestCase):
    def test_push_member_requires_disjoint_ranges_and_index(self):
        f=fixture(56)
        self.assertTrue(validate(f.log,f.receipt,f.artifact)["tessellation_verified"])
        for old,new in (("vertex=0:16","vertex=0:84"), ("control=16:68","control=0:84"),
                        ("selected=1","selected=0")):
            f=fixture(56)
            f.rebuild([r.replace(old,new) for r in f.records])
            with self.assertRaisesRegex(ValueError,"tessellation push member contract"):
                validate(f.log,f.receipt,f.artifact)

    def test_combined_indexed_instances_require_exact_arguments(self):
        for variant in (54,55):
            f=fixture(variant)
            self.assertTrue(validate(f.log,f.receipt,f.artifact)["tessellation_verified"])
            for key in ("indirect", "binding_offset", "first_index", "vertex_offset", "instances", "first_instance"):
                f=fixture(variant)
                f.rebuild([r.replace(key+"=", key+"=bad") if "PS5VK_TESS_INDEXED_INSTANCE " in r else r for r in f.records])
                with self.assertRaisesRegex(ValueError,"tessellation indexed instance contract"):
                    validate(f.log,f.receipt,f.artifact)

    def test_envelope_requires_output_count_and_component_oracle(self):
        for old,new in (("0007c301","00080301"), ("wrong_color=0","wrong_color=1")):
            f=fixture(26)
            self.assertTrue(validate(f.log,f.receipt,f.artifact)["tessellation_verified"])
            f.rebuild([r.replace(old,new) for r in f.records])
            with self.assertRaises(ValueError):validate(f.log,f.receipt,f.artifact)

    def test_indirect_requires_exact_arguments_and_resolver(self):
        f=fixture(25)
        self.assertTrue(validate(f.log,f.receipt,f.artifact)["tessellation_verified"])
        for old,new in (("offset=16","offset=0"), ("instances=2","instances=1"),
                        ("first_instance=3","first_instance=0"),
                        ("resolver=host_submit","resolver=gpu")):
            f=fixture(25)
            f.rebuild([r.replace(old,new) if "PS5VK_TESS_INDIRECT " in r else r for r in f.records])
            with self.assertRaisesRegex(ValueError,"tessellation indirect contract"):
                validate(f.log,f.receipt,f.artifact)

    def test_instances_require_distinct_nonzero_based_identity(self):
        f=fixture(24)
        self.assertTrue(validate(f.log,f.receipt,f.artifact)["tessellation_verified"])
        for old,new in (("count=2","count=1"), ("first=3","first=0"),
                        ("expected_ids=3,4","expected_ids=0,1")):
            f=fixture(24)
            f.rebuild([r.replace(old,new) if "PS5VK_TESS_INSTANCE " in r else r for r in f.records])
            with self.assertRaisesRegex(ValueError,"tessellation instance contract"):
                validate(f.log,f.receipt,f.artifact)

    def test_indexed32_requires_wide_indices_and_signed_offset(self):
        f=fixture(23)
        self.assertTrue(validate(f.log,f.receipt,f.artifact)["tessellation_verified"])
        for old,new in (("type=uint32","type=uint16"), ("vertex_offset=-65536","vertex_offset=0"),
                        ("indices=65541,65538,65543","indices=5,2,7")):
            f=fixture(23)
            f.rebuild([r.replace(old,new) for r in f.records])
            with self.assertRaisesRegex(ValueError,"tessellation indexed contract"):
                validate(f.log,f.receipt,f.artifact)

    def test_indexed_contract_requires_offsets_and_order(self):
        f=fixture(22)
        self.assertTrue(validate(f.log,f.receipt,f.artifact)["tessellation_verified"])
        for old,new in (("type=uint16","type=uint32"), ("binding_offset=2","binding_offset=0"),
                        ("first_index=1","first_index=0"), ("vertex_offset=1","vertex_offset=0"),
                        ("indices=4,1,6","indices=0,1,2")):
            f=fixture(22)
            f.rebuild([r.replace(old,new) for r in f.records])
            with self.assertRaisesRegex(ValueError,"tessellation indexed contract"):
                validate(f.log,f.receipt,f.artifact)

    def test_dense_alpha_cannot_be_inferred_from_rgb(self):
        for old,new in (("samples=2916","samples=9"), ("wrong=0","wrong=1"),
                        ("min=64","min=0"), ("max=64","max=255")):
            f=fixture(18)
            f.rebuild([r.replace(old,new) if "PS5VK_TESS_ALPHA " in r else r for r in f.records])
            with self.assertRaisesRegex(ValueError,"tessellation alpha oracle"):
                validate(f.log,f.receipt,f.artifact)
        f=fixture(18)
        f.rebuild([r for r in f.records if "PS5VK_TESS_ALPHA " not in r])
        with self.assertRaisesRegex(ValueError,"PS5VK_TESS_ALPHA count"):
            validate(f.log,f.receipt,f.artifact)

    def test_dense_color_rejects_lane_errors_and_sparse_coverage(self):
        for variant in (18,19,20,21):
            f=fixture(variant)
            self.assertTrue(validate(f.log,f.receipt,f.artifact)["tessellation_verified"])
            for old,new in (("wrong_color=0","wrong_color=2187"),
                            ("covered=2916","covered=729")):
                broken=fixture(variant)
                broken.rebuild([r.replace(old,new) if "PS5VK_TESS_CONTROL " in r else r
                                for r in broken.records])
                with self.assertRaisesRegex(ValueError,"pixel oracle"):
                    validate(broken.log,broken.receipt,broken.artifact)
    def test_blend_requires_fractional_alpha_overlap_and_matching_draw_count(self):
        for old,new in (("alpha=0.25","alpha=1"),("draws=3","draws=1"),
                        ("draws=6","draws=2"),("op=ADD","op=SUBTRACT"),
                        ("wrong_color=0","wrong_color=9")):
            f=fixture(17)
            f.rebuild([r.replace(old,new) for r in f.records])
            with self.assertRaises(ValueError):validate(f.log,f.receipt,f.artifact)

    def test_geometry_stage_contract_and_pixels_are_both_required(self):
        f = fixture(16)
        self.assertTrue(validate(f.log, f.receipt, f.artifact)["tessellation_verified"])
        for old, new in (("stages=5", "stages=4"), ("shift_x=4", "shift_x=0"),
                         ("color=gbr", "color=rgb"), ("ink=9 ", "ink=10 "),
                         ("wrong_color=0", "wrong_color=9")):
            f = fixture(16)
            f.rebuild([r.replace(old, new) for r in f.records])
            with self.assertRaises(ValueError):
                validate(f.log, f.receipt, f.artifact)
        f = fixture(16)
        f.rebuild([r for r in f.records if "PS5VK_TESS_GEOMETRY" not in r])
        with self.assertRaises(ValueError):
            validate(f.log, f.receipt, f.artifact)

    def test_cache_reuse_requires_hit_without_compile_and_retired_owner(self):
        for old,new in (("hits_delta=1", "hits_delta=0"),
                        ("compiles_delta=0", "compiles_delta=1"),
                        ("original_destroyed=1", "original_destroyed=0")):
            f = fixture(15)
            f.rebuild([r.replace(old,new) for r in f.records])
            with self.assertRaisesRegex(ValueError, "tessellation cache reuse"):
                validate(f.log, f.receipt, f.artifact)
        f = fixture(15)
        f.rebuild([r for r in f.records if "PS5VK_TESS_CACHE_REUSE" not in r])
        with self.assertRaisesRegex(ValueError, "PS5VK_TESS_CACHE_REUSE count"):
            validate(f.log, f.receipt, f.artifact)

    def test_point_mode_rejects_extra_ink_even_with_forged_zero_foreign(self):
        f = fixture(13)
        self.assertTrue(validate(f.log, f.receipt, f.artifact)["tessellation_verified"])
        f.rebuild([r.replace("ink=9 ", "ink=10 ") for r in f.records])
        with self.assertRaisesRegex(ValueError, "point-mode exact coverage"):
            validate(f.log, f.receipt, f.artifact)

    def test_asymmetric_launch_must_not_repeat_input_count(self):
        f = fixture(11)
        self.assertTrue(validate(f.log, f.receipt, f.artifact)["tessellation_verified"])
        f.rebuild([r.replace("00080301", "0000c301") for r in f.records])
        with self.assertRaisesRegex(ValueError, "asymmetric launch counts"):
            validate(f.log, f.receipt, f.artifact)

    def test_two_pipeline_claim_needs_draw_evidence(self):
        f = fixture()
        f.artifact["tessellation_witness"]["shared_pipelines"] = 2
        with self.assertRaises(ValueError):
            validate(f.log, f.receipt, f.artifact)
        records = list(f.records)
        at = next(i for i,r in enumerate(records) if "TESS_QUEUE_RING_BOUND" in r)
        records[at:at] = ["PS5VK_TESS_SHARED_STORAGE pipelines=2 same_storage=1 split_scissors=1",
                         "PS5VK_GRAPHICS_PREPARED serial=20 draws=2"]
        f.rebuild(records)
        self.assertTrue(validate(f.log, f.receipt, f.artifact)["shared_pipelines_verified"])
        f.rebuild([r.replace("draws=2", "draws=1") for r in records])
        with self.assertRaises(ValueError):
            validate(f.log, f.receipt, f.artifact)

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
