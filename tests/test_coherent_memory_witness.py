"""Contracts for the HOST_COHERENT memory witness verifier."""

import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_coherent_memory_witness import PROFILE, SWITCHES, WORDS  # noqa: E402
from run_coherent_memory_witness import verify  # noqa: E402

ARTIFACT = {"profile": PROFILE, "words": WORDS, "sdk_switches": SWITCHES,
            "eboot_sha256": "0" * 64}


def fixture_log(*, types="2 type0=3 type1=7 coherent_type=1", c1=0, c2=(0, 0),
                c3=(0, 0, 0), control=(0, 5, 5, "stale-observed"), retired="clean",
                failure=False, swap=False):
    cases = [f"COHERENT_WITNESS_RESULT case=C1 mismatches={c1}",
             f"COHERENT_WITNESS_RESULT case=C2 mismatches={c2[0]} stale={c2[1]}",
             f"COHERENT_WITNESS_RESULT case=C3 mismatches={c3[0]} stale={c3[1]} "
             f"host_half={c3[2]}"]
    if swap:
        cases.reverse()
    lines = [f"COHERENT_WITNESS_START types={types} words={WORDS}", *cases,
             "COHERENT_WITNESS_CONTROL maintained_mismatches={} unmaintained_mismatches={} "
             "stale_host_read={} verdict={}".format(*control),
             f"COHERENT_WITNESS_RETIRED resources={retired}"]
    if failure:
        lines.append("COHERENT_WITNESS_FAILURE call=x result=-1 retirement=attempted")
    return ("\n".join(lines) + "\n").encode()


def receipt(log):
    return {"protocol": "ps5log/1", "title": "PPSA99994", "app": "ps5vk",
            "transport": "tcp", "clean": True, "bye": True, "gaps": 0,
            "sha256": hashlib.sha256(log).hexdigest(), "run_id": "run"}


class CoherentWitnessVerifier(unittest.TestCase):
    def check(self, log):
        return verify(log, receipt(log), ARTIFACT)

    def test_accepts_the_exact_passing_shape(self):
        result = self.check(fixture_log())
        self.assertTrue(result["strict_verified"])
        self.assertEqual("stale-observed", result["control_verdict"])
        self.assertEqual(5, result["control_unmaintained_mismatches"])
        quiet = self.check(fixture_log(control=(0, 0, 0, "no-stale-observed")))
        self.assertEqual("no-stale-observed", quiet["control_verdict"])

    def test_rejects_lost_coherent_data(self):
        for log in (fixture_log(c1=1), fixture_log(c2=(3, 3)), fixture_log(c3=(2, 0, 2))):
            with self.assertRaises(ValueError):
                self.check(log)

    def test_rejects_wrong_profile_control_and_lifecycle(self):
        for log in (fixture_log(types="1 type0=3 type1=0 coherent_type=-1"),
                    fixture_log(types="2 type0=7 type1=7 coherent_type=0"),
                    fixture_log(control=(1, 5, 5, "instrument-broken")),
                    fixture_log(control=(0, 5, 5, "no-stale-observed")),
                    fixture_log(retired="leaked"), fixture_log(failure=True),
                    fixture_log(swap=True)):
            with self.assertRaises(ValueError):
                self.check(log)

    def test_rejects_corrupt_receipt_and_foreign_artifact(self):
        log = fixture_log()
        bad = receipt(log)
        bad["sha256"] = "1" * 64
        with self.assertRaises(ValueError):
            verify(log, bad, ARTIFACT)
        with self.assertRaises(ValueError):
            verify(log, receipt(log), dict(ARTIFACT, sdk_switches={"PS5VK_X": "1"}))

    def test_coherent_type_ships_on_the_ordinary_profile(self):
        platform = (ROOT / "native/platform_ps5.c").read_text()
        self.assertIn("platform->supported_features_t09 |= PS5VK_T09_FEATURE_HOST_COHERENT_MEMORY;\n"
                      "    ps5vk_profile_add_coherent_type(&platform->memory_properties);\n"
                      "    return VK_SUCCESS;", platform)
        self.assertEqual({}, SWITCHES)
        source = (ROOT / "examples/coherent_memory_witness/main.c").read_text()
        # The coherent cases never use the explicit cache commands on the
        # coherent region; only the non-coherent control does.
        self.assertEqual(1, source.count("vkFlushMappedMemoryRanges("))
        self.assertEqual(1, source.count("vkInvalidateMappedMemoryRanges("))
        self.assertIn(".memory = control.memory", source)


if __name__ == "__main__":
    unittest.main()
