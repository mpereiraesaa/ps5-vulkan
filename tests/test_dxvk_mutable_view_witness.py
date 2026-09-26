"""Contracts for the public-SDK mutable-format view witness and its switch."""

import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_mutable_view_witness import (  # noqa: E402
    PIXELS, PROFILE, SWITCH, data_header, min_differing, srgb_decode_code, srgb_exact,
    texels)
from run_dxvk_mutable_view_witness import fnv1a, verify  # noqa: E402


def fixture_artifact(**changes):
    artifact = {"profile": PROFILE, "extent": 16, "diagnostic_switch": SWITCH,
                "min_differing": min_differing(),
                "texels_sha256": hashlib.sha256(texels()).hexdigest(),
                "srgb_exact_sha256": hashlib.sha256(srgb_exact()).hexdigest(),
                "eboot_sha256": "0" * 64}
    artifact.update(changes)
    return artifact


def fixture_log(*, srgb32="0000d801", srgb64=None, refusal=-8, native=0, over_one=0,
                differing=None, digest_srgb="1234abcd", digest_native="1234abcd",
                failure=False, retired=True):
    differing = min_differing() if differing is None else differing
    srgb64 = srgb64 or srgb32.rjust(16, "0")
    lines = [
        "DXVK_MUTABLE_VIEW_WITNESS_START extent=16 flags2_spec=2 list_spec=1"
        " unorm32=0000d981 unorm64=000000000000d981"
        f" srgb32={srgb32} srgb64={srgb64} rt_query=0 rt_max=16384x16384",
        f"DXVK_MUTABLE_VIEW_WITNESS_REFUSAL view=target_srgb result={refusal}",
        f"DXVK_MUTABLE_VIEW_WITNESS_RESULT bytes=1024 unorm_mismatches=0"
        f" srgb_vs_native={native} srgb_over_one={over_one} srgb_exact=1000"
        f" alpha_mismatches=0 srgb_vs_unorm_differing={differing}"
        f" digest_unorm={fnv1a(texels()):08x} digest_srgb={digest_srgb}"
        f" digest_native={digest_native}",
    ]
    if failure:
        lines.append("DXVK_MUTABLE_VIEW_WITNESS_FAILURE call=x result=-1")
    if retired:
        lines.append("DXVK_MUTABLE_VIEW_WITNESS_RETIRED resources=clean")
    return ("\n".join(lines) + "\n").encode()


def receipt_for(log):
    return {"protocol": "ps5log/1", "title": "PPSA99994", "app": "ps5vk",
            "transport": "tcp", "clean": True, "bye": True, "gaps": [],
            "sha256": hashlib.sha256(log).hexdigest(), "run_id": "fixture"}


class MutableViewWitness(unittest.TestCase):
    def test_oracle_is_the_exact_srgb_decode(self):
        self.assertEqual([srgb_decode_code(c) for c in (0, 1, 10, 64, 128, 188, 254, 255)],
                         [0, 0, 1, 13, 55, 128, 253, 255])
        source, decoded = texels(), srgb_exact()
        self.assertEqual(len(source), PIXELS * 4)
        for channel in range(3):
            self.assertEqual(sorted(source[channel::4]), list(range(256)))
        self.assertEqual(decoded[3::4], source[3::4])
        self.assertEqual(min_differing(), 756)
        header = data_header()
        self.assertIn("mutable_view_texels[]", header)
        self.assertIn("mutable_view_srgb_exact[]", header)
        self.assertIn("#define MUTABLE_VIEW_MIN_DIFFERING 756u", header)

    def test_verify_accepts_the_contract(self):
        log = fixture_log()
        result = verify(log, receipt_for(log), fixture_artifact())
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["srgb_vs_unorm_differing"], 756)

    def test_verify_rejects_each_broken_axis(self):
        broken = [
            fixture_log(srgb64="000000010000d801"),      # 64-bit-only bit claimed
            fixture_log(srgb32="0000d881"),              # SRGB colour attachment claimed
            fixture_log(refusal=0),                      # SRGB RT view created
            fixture_log(native=1),                       # view differs from SRGB image
            fixture_log(over_one=1),                     # outside the decode tolerance
            fixture_log(differing=min_differing() - 1),  # decode not applied
            fixture_log(digest_native="00000000"),
            fixture_log(failure=True),
            fixture_log(retired=False),
        ]
        for log in broken:
            with self.subTest(log=log[:80]):
                with self.assertRaises(ValueError):
                    verify(log, receipt_for(log), fixture_artifact())
        log = fixture_log()
        with self.assertRaises(ValueError):
            verify(log, receipt_for(log), fixture_artifact(min_differing=1))
        with self.assertRaises(ValueError):
            verify(log, dict(receipt_for(log), sha256="0" * 64), fixture_artifact())

    def test_routes_are_shipping(self):
        self.assertIsNone(SWITCH)
        from check_dxvk_profile import implemented_device_extensions
        shipping = implemented_device_extensions()
        self.assertIn("VK_KHR_format_feature_flags2", shipping)
        self.assertIn("VK_KHR_image_format_list", shipping)


if __name__ == "__main__":
    unittest.main()
