"""Contracts for the isolated DXVK262-T13 public-SDK robustness2 witness."""

import hashlib
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t13_robustness2_witness import (  # noqa: E402
    PROFILE, SDK_SWITCHES, SHADERS, checked_spirv)
from run_t13_robustness2_witness import ACCESS_WORDS, expected_word, verify  # noqa: E402


def instruction(opcode, *operands):
    return [(len(operands) + 1) << 16 | opcode, *operands]


def fixture_spirv(capabilities=(1, 46), model=(0, 1), version=0x00010000):
    words = [0x07230203, version, 0, 8, 0]
    for capability in capabilities:
        words += instruction(17, capability)
    words += instruction(14, *model)
    return struct.pack(f"<{len(words)}I", *words)


def fixture_log(*, words=None, access=None, query="created marker=c0de0014 image_width=0 "
                "image_height=0 texel_size=0", fence="0", alignment="4",
                retired=True, failure=False, swap=False):
    values = words or [expected_word(n) for n in range(ACCESS_WORDS)]
    access = access or ("c0de0013", "26", "0", "-1", "0", "0", "5a5a0001", "0000100c",
                        "0000103c")
    word_lines = [f"T13_ROBUSTNESS2_WITNESS_WORD index={n} value={values[n]:08x} "
                  f"expected={expected_word(n):08x}" for n in range(ACCESS_WORDS)]
    lines = [
        f"T13_ROBUSTNESS2_WITNESS_START spec=1 storage_alignment={alignment} "
        "uniform_alignment=4 storage_range=38 uniform_range=40",
        f"T13_ROBUSTNESS2_WITNESS_FENCE result={fence} slices=1 query_pipeline=0",
        *word_lines,
        "T13_ROBUSTNESS2_WITNESS_ACCESS marker={} words={} mismatches={} first={} "
        "guard_mismatches={} store_mismatches={} in_range_store={} dropped_store_12={} "
        "dropped_store_60={}".format(*access),
        f"T13_ROBUSTNESS2_WITNESS_QUERY pipeline={query}",
    ]
    if swap:
        lines[-1], lines[-2] = lines[-2], lines[-1]
    if retired:
        lines.append("T13_ROBUSTNESS2_WITNESS_RETIRED resources=clean")
    if failure:
        lines.append("T13_ROBUSTNESS2_WITNESS_FAILURE call=x result=-1 retirement=attempted")
    return ("\n".join(lines) + "\n").encode()


def fixture_receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk",
                transport="tcp", clean=True, bye=True, gaps=[],
                sha256=hashlib.sha256(log).hexdigest(), run_id="unit-run")


def fixture_artifact():
    return dict(profile=PROFILE, storage_range=38, uniform_range=40,
                sdk_switches=dict(SDK_SWITCHES), eboot_sha256="0" * 64)


class SpirvContract(unittest.TestCase):
    def test_declared_capabilities(self):
        self.assertEqual(SHADERS["t13_access_spirv"][1], {1, 46})
        self.assertEqual(SHADERS["t13_query_spirv"][1], {1, 46, 50})
        checked_spirv(fixture_spirv(), {1, 46})
        checked_spirv(fixture_spirv((1, 46, 50)), {1, 46, 50})

    def test_other_modules_rejected(self):
        for payload in (fixture_spirv((1, 46, 50)), fixture_spirv((1,)),
                        fixture_spirv(model=(5348, 1)),
                        fixture_spirv(version=0x00010300), fixture_spirv()[:-1]):
            with self.subTest(size=len(payload)):
                with self.assertRaises(ValueError):
                    checked_spirv(payload, {1, 46})


class Oracle(unittest.TestCase):
    def test_oracle_words(self):
        # In-range reads, the dword inside the 4-byte-rounded 38-byte range,
        # and the (x, y) half of the uniform vec4 straddling 40 bytes.
        self.assertEqual([expected_word(n) for n in (0, 1, 2, 5, 6, 13)],
                         [0x1000, 0x1008, 0x1009, 0x2008, 0x2009, 0x2000])
        # Out of range and every null descriptor read zero.
        for n in (3, 4, 7, 8, 9, 10, 11, 12, *range(14, ACCESS_WORDS)):
            self.assertEqual(expected_word(n), 0)


class LogContract(unittest.TestCase):
    def test_exact_log_verifies(self):
        log = fixture_log()
        result = verify(log, fixture_receipt(log), fixture_artifact())
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["query"], {"pipeline": "created", "image_size": [0, 0],
                                           "texel_size": 0})
        self.assertEqual(result["robust_access_size_alignment"],
                         {"storage": 4, "uniform": 4})

    def test_null_image_alpha_one_is_accepted(self):
        words = [expected_word(n) for n in range(ACCESS_WORDS)]
        words[25] = 1
        log = fixture_log(words=words)
        self.assertTrue(verify(log, fixture_receipt(log), fixture_artifact())["strict_verified"])
        words[24] = 1
        log = fixture_log(words=words)
        with self.assertRaises(ValueError):
            verify(log, fixture_receipt(log), fixture_artifact())

    def test_refused_query_pipeline_is_reported(self):
        log = fixture_log(query="refused result=-7")
        result = verify(log, fixture_receipt(log), fixture_artifact())
        self.assertEqual(result["query"], {"pipeline": "refused"})

    def test_oracle_failures_rejected(self):
        exact = [expected_word(n) for n in range(ACCESS_WORDS)]
        bad_words = []
        for index, value in ((2, 0), (3, 0x100a), (7, 0x200a), (14, 0xbad00003), (24, 1),
                             (25, 2)):
            words = list(exact)
            words[index] = value
            bad_words.append(words)
        bad_access = [("c0de0013", "26", "0", "-1", "0", "1", "5a5a0001", "0000100c",
                       "0000103c"),
                      ("c0de0013", "26", "0", "-1", "0", "0", "5a5a0001", "bad0000c",
                       "0000103c"),
                      ("c0de0013", "26", "0", "-1", "1", "0", "5a5a0001", "0000100c",
                       "0000103c"),
                      ("00000000", "26", "0", "-1", "0", "0", "5a5a0001", "0000100c",
                       "0000103c")]
        cases = ([dict(words=w) for w in bad_words] + [dict(access=a) for a in bad_access] +
                 [dict(query="created marker=c0de0014 image_width=1 image_height=1 "
                       "texel_size=0"),
                  dict(fence="2"), dict(alignment="1"), dict(retired=False),
                  dict(failure=True), dict(swap=True)])
        for kwargs in cases:
            with self.subTest(kwargs=kwargs):
                log = fixture_log(**kwargs)
                with self.assertRaises(ValueError):
                    verify(log, fixture_receipt(log), fixture_artifact())

    def test_receipt_and_artifact_identity_required(self):
        log = fixture_log()
        receipt = fixture_receipt(log)
        receipt["sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            verify(log, receipt, fixture_artifact())
        artifact = fixture_artifact()
        artifact["sdk_switches"] = {}
        with self.assertRaises(ValueError):
            verify(log, fixture_receipt(log), artifact)


if __name__ == "__main__":
    unittest.main()
