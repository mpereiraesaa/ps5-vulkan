"""Contracts for the public-SDK separate-sampler and texel-buffer witness."""

import hashlib
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_separate_sampler_witness import (  # noqa: E402
    HEIGHT, PROFILE, SWITCH, WIDTH, data_header, expected_pixels, expected_rgba32f,
    expected_rgba8, expected_u32, fetch_values, texel, texture)
from run_dxvk_separate_sampler_witness import expected_digests, verify  # noqa: E402


def fixture_artifact(**changes):
    artifact = {"profile": PROFILE, "width": WIDTH, "height": HEIGHT,
                "diagnostic_switch": SWITCH,
                "data_sha256": hashlib.sha256(data_header().encode()).hexdigest(),
                "eboot_sha256": "0" * 64}
    artifact.update(changes)
    return artifact


def fixture_log(*, rgba8="00000118", r32i="00000108", refusal=-8, u32=0, pixels=0,
                digests=None, failure=False, retired=True):
    digests = digests or expected_digests()
    lines = [
        f"DXVK_SEPARATE_SAMPLER_WITNESS_START width={WIDTH} height={HEIGHT}"
        f" r32ui=00000118 rgba8={rgba8} rgba32f=00000118 r32i={r32i}",
        f"DXVK_SEPARATE_SAMPLER_WITNESS_REFUSAL view=r32i_storage_texel result={refusal}",
        f"DXVK_SEPARATE_SAMPLER_WITNESS_RESULT u32_mismatches={u32} rgba8_mismatches=0"
        f" rgba32f_mismatches=0 pixel_mismatches={pixels}"
        f" digest_u32={digests[0]:08x} digest_rgba8={digests[1]:08x}"
        f" digest_rgba32f={digests[2]:08x} digest_pixels={digests[3]:08x}",
    ]
    if failure:
        lines.append("DXVK_SEPARATE_SAMPLER_WITNESS_FAILURE call=x result=-1")
    if retired:
        lines.append("DXVK_SEPARATE_SAMPLER_WITNESS_RETIRED resources=clean")
    return ("\n".join(lines) + "\n").encode()


def receipt_for(log):
    return {"protocol": "ps5log/1", "title": "PPSA99994", "app": "ps5vk",
            "transport": "tcp", "clean": True, "bye": True, "gaps": [],
            "sha256": hashlib.sha256(log).hexdigest(), "run_id": "fixture"}


class SeparateSamplerWitness(unittest.TestCase):
    def test_oracle(self):
        data = texture()
        self.assertEqual(len(data), WIDTH * HEIGHT * 4)
        for channel in range(4):
            values = data[channel::4]
            self.assertEqual(len(set(values)), len(values), "channel bytes are distinct")
        self.assertEqual(texel(3, 1), data[4 * (WIDTH + 3):4 * (WIDTH + 3) + 4])
        # packUnorm4x8 of an unorm8 texel is its little-endian word.
        word = struct.unpack("<I", texel(5, 1))[0]
        self.assertEqual(expected_u32()[5], word ^ fetch_values()[5] ^ WIDTH)
        self.assertEqual(expected_rgba8(), b"".join(texel(i, 2) for i in range(WIDTH)))
        self.assertEqual(struct.unpack("<4f", expected_rgba32f()[16 * 7:16 * 8]),
                         (float(fetch_values()[7]), 8.0, 4.0, 7.0))
        self.assertTrue(all(value < (1 << 24) for value in fetch_values()))
        pixels = expected_pixels()
        self.assertEqual(pixels[4 * (2 * WIDTH + 6):4 * (2 * WIDTH + 6) + 4],
                         texel(6, 2)[:3] + bytes((fetch_values()[6] & 0xff,)))
        header = data_header()
        for name in ("separate_sampler_texture[]", "separate_sampler_fetch[]",
                     "separate_sampler_expected_u32[]", "separate_sampler_expected_pixels[]"):
            self.assertIn(name, header)

    def test_verify_accepts_the_contract(self):
        log = fixture_log()
        result = verify(log, receipt_for(log), fixture_artifact())
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["buffer_features"]["r32i"], "00000108")

    def test_verify_rejects_each_broken_axis(self):
        wrong = list(expected_digests())
        wrong[3] ^= 1
        broken = [
            fixture_log(rgba8="00000108"),     # a storage-texel row not reported
            fixture_log(r32i="00000118"),      # R32_SINT claimed storage-texel
            fixture_log(refusal=0),            # R32_SINT view created
            fixture_log(u32=1),                # compute oracle mismatch
            fixture_log(pixels=4),             # graphics oracle mismatch
            fixture_log(digests=tuple(wrong)), # digest disagrees with the oracle
            fixture_log(failure=True),
            fixture_log(retired=False),
        ]
        for log in broken:
            with self.assertRaises(ValueError):
                verify(log, receipt_for(log), fixture_artifact())
        log = fixture_log()
        with self.assertRaises(ValueError):
            verify(log, receipt_for(log), fixture_artifact(diagnostic_switch="OTHER"))
        receipt = receipt_for(log)
        receipt["clean"] = False
        with self.assertRaises(ValueError):
            verify(log, receipt, fixture_artifact())

    def test_switch_is_default_off_and_forwarded(self):
        sdk = (ROOT / "tools/build_sdk.py").read_text()
        self.assertIn('"PS5VK_STORAGE_TEXEL_DIAGNOSTIC"', sdk)
        rows = (ROOT / "src/texture_format.c").read_text()
        self.assertIn("#if defined(PS5VK_STORAGE_TEXEL_DIAGNOSTIC) && "
                      "PS5VK_STORAGE_TEXEL_DIAGNOSTIC", rows)
        self.assertIn("#define STEXEL_ENABLED 0u", rows)


if __name__ == "__main__":
    unittest.main()
