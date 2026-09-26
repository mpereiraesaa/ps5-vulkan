"""Contracts for the public-SDK DXVK routes witness (memory requirements2,
dedicated allocation, bind2 and descriptor update templates)."""

import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_routes_witness import PROFILE, ROUTE_SWITCHES, SEEDS, TARGETS, VALUES  # noqa: E402
from run_dxvk_routes_witness import expected_results, verify  # noqa: E402


def fixture_log(results=None, stride=256, agrees=(1, 1), retired=True, failure=False):
    rows = expected_results() if results is None else results
    lines = [f"DXVK_ROUTES_WITNESS_START memreq2=1 dedicated=3 bind2=1 template=1 commands=7 "
             f"values={VALUES} targets={TARGETS}",
             f"DXVK_ROUTES_WITNESS_REQUIREMENTS buffer_size=384 buffer_alignment=256 "
             f"buffer_agrees={agrees[0]} buffer_prefers=0 buffer_requires=0 image_size=65536 "
             f"image_alignment=65536 image_agrees={agrees[1]} image_prefers=0 image_requires=0",
             f"DXVK_ROUTES_WITNESS_BIND dedicated_buffer=1 dedicated_image=1 shared_binds=2 "
             f"shared_offset={stride}"]
    lines += [f"DXVK_ROUTES_WITNESS_RESULT dispatch={d} target={t} mismatches={m} "
              f"guard_mismatches={g} fence={f}" for d, t, m, g, f in rows]
    if failure:
        lines.append("DXVK_ROUTES_WITNESS_FAILURE call=x result=-3 completed=1")
    if retired:
        lines.append(f"DXVK_ROUTES_WITNESS_RETIRED resources=clean dispatches={TARGETS}")
    return ("\n".join(lines) + "\n").encode()


def receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk", transport="tcp",
                clean=True, bye=True, gaps=[], sha256=hashlib.sha256(log).hexdigest(),
                run_id="unit-run")


def artifact(**changes):
    base = dict(profile=PROFILE, values=VALUES, targets=TARGETS, seeds=list(SEEDS),
                sdk_switches=dict(ROUTE_SWITCHES), eboot_sha256="0" * 64)
    base.update(changes)
    return base


class RoutesWitnessContract(unittest.TestCase):
    def test_complete_run_verifies(self):
        log = fixture_log(stride=512)
        result = verify(log, receipt(log), artifact())
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["shared_offset"], 512)
        self.assertEqual(result["spec_versions"], [1, 3, 1, 1])

    def test_every_dispatch_checks_its_target_and_the_last_checks_all(self):
        rows = expected_results()
        self.assertEqual([r[:2] for r in rows],
                         [("0", "0"), ("1", "1"), ("2", "0"), ("2", "1"), ("2", "2")])

    def test_defects_are_rejected(self):
        bad_rows = list(expected_results())
        bad_rows[1] = ("1", "1", "3", "0", "complete")
        for log, art in ((fixture_log(results=bad_rows, stride=512), artifact()),
                         (fixture_log(stride=384), artifact()),
                         (fixture_log(stride=512, agrees=(0, 1)), artifact()),
                         (fixture_log(stride=512, retired=False), artifact()),
                         (fixture_log(stride=512, failure=True), artifact()),
                         (fixture_log(stride=512), artifact(targets=2)),
                         (fixture_log(stride=512), artifact(sdk_switches={"PS5VK_X": "1"}))):
            with self.subTest(art=art):
                with self.assertRaises(ValueError):
                    verify(log, receipt(log), art)

    def test_payload_is_bounded_and_public(self):
        source = (ROOT / "examples/dxvk_routes_witness/main.c").read_text()
        self.assertIn("fence_timeout = UINT64_C(300000000)", source)
        self.assertNotIn('#include "vk_', source)
        for command in ("vkGetBufferMemoryRequirements2KHR", "vkGetImageMemoryRequirements2KHR",
                        "vkBindBufferMemory2KHR", "vkBindImageMemory2KHR",
                        "vkUpdateDescriptorSetWithTemplateKHR"):
            self.assertIn(command, source)
        self.assertNotIn("vkUpdateDescriptorSets(", source)


if __name__ == "__main__":
    unittest.main()
