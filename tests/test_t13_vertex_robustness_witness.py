"""Contracts for the public-SDK robustness2 vertex-input witness verifier."""

import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_t13_vertex_robustness_witness import PROFILE, verify  # noqa: E402


def fixture_log(colors=("00000000", "000000ff", "4080bfff"), uniform=(0, 0, 0),
                retired=True, failure=False):
    lines = ["T13_VERTEX_WITNESS_START width=48 height=16 columns=3"]
    lines += [f"T13_VERTEX_WITNESS_COLUMN column={i} rgba={c} uniform_mismatches={u}"
              for i, (c, u) in enumerate(zip(colors, uniform))]
    lines.append("T13_VERTEX_WITNESS_RESULT submissions=2 fence=complete")
    if failure:
        lines.append("T13_VERTEX_WITNESS_FAILURE result=-3 step=x completed=1")
    if retired:
        lines.append("T13_VERTEX_WITNESS_RETIRED resources=clean")
    return ("\n".join(lines) + "\n").encode()


def receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk", transport="tcp",
                clean=True, bye=True, gaps=[], sha256=hashlib.sha256(log).hexdigest(),
                run_id="unit-run")


ARTIFACT = dict(profile=PROFILE, width=48, height=16, columns=3, eboot_sha256="0" * 64)


class VertexRobustnessContract(unittest.TestCase):
    def test_zero_reads_and_control_verify(self):
        log = fixture_log()
        result = verify(log, receipt(log), ARTIFACT)
        self.assertEqual(result["zero_alpha"], [0, 255])
        log = fixture_log(colors=("000000ff", "00000000", "4081bffe"))
        self.assertTrue(verify(log, receipt(log), ARTIFACT)["strict_verified"])

    def test_defects_are_rejected(self):
        for log in (fixture_log(colors=("01000000", "00000000", "4080bfff")),
                    fixture_log(colors=("00000000", "00000080", "4080bfff")),
                    fixture_log(colors=("00000000", "00000000", "ff00ff80")),
                    fixture_log(uniform=(0, 3, 0)),
                    fixture_log(retired=False), fixture_log(failure=True)):
            with self.assertRaises(ValueError):
                verify(log, receipt(log), ARTIFACT)

    def test_payload_is_bounded_and_public(self):
        source = (ROOT / "examples/t13_vertex_robustness_witness/main.c").read_text()
        self.assertIn("fence_timeout = UINT64_C(300000000)", source)
        self.assertNotIn('#include "vk_', source)
        self.assertIn("VK_NULL_HANDLE", source)


if __name__ == "__main__":
    unittest.main()
