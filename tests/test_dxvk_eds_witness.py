"""Contracts for the public-SDK extended-dynamic-state witness verifier."""

import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from run_dxvk_eds_witness import EXPECTED, PROFILE, verify  # noqa: E402

GOOD = tuple((t, s, v, c, 0) for t, s, v, c in EXPECTED)


def fixture_log(columns=GOOD, retired=True, failure=False):
    lines = ["DXVK_EDS_WITNESS_START width=128 height=16 columns=8"]
    lines += [f"DXVK_EDS_WITNESS_COLUMN column={i} topology={t} stride={s} vertices={v} "
              f"rgba={c} uniform_mismatches={u}" for i, (t, s, v, c, u) in enumerate(columns)]
    lines.append("DXVK_EDS_WITNESS_RESULT submissions=2 fence=complete")
    if failure:
        lines.append("DXVK_EDS_WITNESS_FAILURE result=-3 step=x completed=1")
    if retired:
        lines.append("DXVK_EDS_WITNESS_RETIRED resources=clean")
    return ("\n".join(lines) + "\n").encode()


def receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk", transport="tcp",
                clean=True, bye=True, gaps=[], sha256=hashlib.sha256(log).hexdigest(),
                run_id="unit-run")


ARTIFACT = dict(profile=PROFILE, width=128, height=16, columns=8, eboot_sha256="0" * 64)


def replace(column, **changes):
    rows = [list(row) for row in GOOD]
    for key, value in changes.items():
        rows[column][("topology", "stride", "vertices", "rgba", "uniform").index(key)] = value
    return tuple(tuple(row) for row in rows)


class EdsWitnessContract(unittest.TestCase):
    def test_exact_columns_verify(self):
        log = fixture_log()
        result = verify(log, receipt(log), ARTIFACT)
        self.assertEqual(result["columns"], ["ff0000ff", "00ff00ff", "0000ffff", "4080c0ff",
                                             "ffff00ff", "ff00ffff", "00ffffff", "804020ff"])

    def test_defects_are_rejected(self):
        for log in (fixture_log(replace(1, uniform=128)),     # strip drawn as a list
                    fixture_log(replace(3, rgba="abababab")),  # wrong stride
                    fixture_log(replace(2, stride=16)),
                    fixture_log(replace(1, topology=3)),
                    fixture_log(replace(7, rgba="101010ff")),  # adjacency lost
                    fixture_log(replace(5, uniform=40)),       # adjacency drawn
                    fixture_log(retired=False), fixture_log(failure=True)):
            with self.assertRaises(ValueError):
                verify(log, receipt(log), ARTIFACT)

    def test_payload_is_bounded_and_public(self):
        source = (ROOT / "examples/dxvk_eds_witness/main.c").read_text()
        self.assertIn("fence_timeout = UINT64_C(300000000)", source)
        self.assertNotIn('#include "vk_', source)
        self.assertIn("VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT", source)
        self.assertIn("VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT", source)
        self.assertIn("VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN", source)
        self.assertIn("VK_SHADER_STAGE_GEOMETRY_BIT", source)


if __name__ == "__main__":
    unittest.main()
