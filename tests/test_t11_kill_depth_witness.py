"""Contracts for the T11 public-SDK pixel-removal witness builder and verifier."""

import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t11_kill_depth_witness import (  # noqa: E402
    SHADERS, checked_spirv, export_memory_switch)
from run_t11_kill_depth_witness import (  # noqa: E402
    CASES, REMOVED, SUBMISSIONS_PER_CASE, verify)

CLEAN = dict(depth=0, stencil=0, written=0, missing=0)


def case_line(form, removes, depth=0, stencil=0, written=0, missing=0, fence="complete"):
    return (f"T11_KILL_WITNESS_CASE form={form} removes={removes} depth_mismatches={depth} "
            f"stencil_mismatches={stencil} removed_written={written} kept_missing={missing} "
            f"depth_0_0=39000000 depth_1_0=3f800000 stencil_0_0=5a stencil_1_0=a5 "
            f"depth_digest=0badf00d stencil_digest=feedface fence={fence}")


def fixture_log(switch="1", tallies=None, retired=True, failure=False, forms=CASES,
                steps=SUBMISSIONS_PER_CASE):
    tallies = tallies or {}
    lines = [f"T11_KILL_WITNESS_START extent=64 export_memory={switch}"]
    lines += [f"T11_KILL_WITNESS_PIPELINE form={form} created=1" for form in forms]
    for form in forms:
        lines += [f"T11_KILL_WITNESS_STEP form={form} index={i} fence=complete"
                  for i in range(steps)]
        lines.append(case_line(form, int(form != "control"), **tallies.get(form, CLEAN)))
    lines.append(f"T11_KILL_WITNESS_RESULT cases={len(forms)} "
                 f"submissions={len(forms) * steps} fence=complete")
    if failure:
        lines.append("T11_KILL_WITNESS_FAILURE result=-3 step=x completed=4")
    if retired:
        lines.append("T11_KILL_WITNESS_RETIRED resources=clean")
    return ("\n".join(lines) + "\n").encode()


def receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk", transport="tcp",
                clean=True, bye=True, gaps=[], sha256=hashlib.sha256(log).hexdigest(),
                run_id="unit-run")


def artifact(switch="1"):
    return dict(profile="t11-kill-depth-public-sdk-witness", extent=64,
                format="D32_SFLOAT_S8_UINT", cases=list(CASES),
                diagnostic_switch={"PS5VK_KILL_EXPORT_MEMORY": switch},
                eboot_sha256="artifact-sha")


class Verifier(unittest.TestCase):
    def test_suppressed_removal_is_a_verified_measurement(self):
        log = fixture_log()
        result = verify(log, receipt(log), artifact())
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["export_memory"], "1")
        self.assertEqual(result["verdict"], {"kill": "removal-suppressed",
                                             "terminate": "removal-suppressed",
                                             "demote": "removal-suppressed"})

    def test_the_open_defect_is_a_verified_measurement_too(self):
        ignored = dict(depth=REMOVED, stencil=REMOVED, written=REMOVED, missing=0)
        log = fixture_log(switch="0", tallies={f: ignored for f in CASES[1:]})
        result = verify(log, receipt(log), artifact("0"))
        self.assertEqual(set(result["verdict"].values()), {"removal-ignored"})
        self.assertEqual(result["tallies"]["kill"]["removed_written"], 2048)

    def test_partial_and_lost_pixels_are_named(self):
        tallies = {"kill": dict(depth=5, stencil=0, written=5, missing=0),
                   "demote": dict(depth=3, stencil=0, written=0, missing=3)}
        log = fixture_log(tallies=tallies)
        verdict = verify(log, receipt(log), artifact())["verdict"]
        self.assertEqual(verdict["kill"], "removal-partial")
        self.assertEqual(verdict["demote"], "kept-pixels-lost")
        self.assertEqual(verdict["terminate"], "removal-suppressed")

    def test_invalid_runs_are_rejected(self):
        broken_control = {"control": dict(depth=1, stencil=0, written=0, missing=1)}
        cases = [
            (fixture_log(tallies=broken_control), artifact()),
            (fixture_log(switch="0"), artifact("1")),
            (fixture_log(retired=False), artifact()),
            (fixture_log(failure=True), artifact()),
            (fixture_log(forms=CASES[:3]), artifact()),
            (fixture_log(steps=1), artifact()),
            (fixture_log(), dict(artifact(), cases=["kill"])),
            (fixture_log(), dict(artifact(), diagnostic_switch={})),
        ]
        for log, art in cases:
            with self.subTest(art=art):
                with self.assertRaises(ValueError):
                    verify(log, receipt(log), art)
        log = fixture_log()
        bad = receipt(log)
        bad["sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            verify(log, bad, artifact())


class Builder(unittest.TestCase):
    def test_switch_values(self):
        self.assertEqual(export_memory_switch({}), "0")
        self.assertEqual(export_memory_switch({"PS5VK_KILL_EXPORT_MEMORY": "1"}), "1")
        with self.assertRaises(SystemExit):
            export_memory_switch({"PS5VK_KILL_EXPORT_MEMORY": "yes"})

    @unittest.skipUnless(shutil.which("glslangValidator"), "glslangValidator unavailable")
    def test_each_case_is_exactly_its_form(self):
        compiled = {}
        with tempfile.TemporaryDirectory() as directory:
            for name, (source, arguments, version, capabilities, removal) in SHADERS.items():
                target = Path(directory) / f"{name}.spv"
                subprocess.run(["glslangValidator", "-V", *arguments, str(ROOT / source),
                                "-o", str(target)], check=True, capture_output=True)
                compiled[name] = target.read_bytes()
                checked_spirv(compiled[name], version, capabilities, removal)
        # A module is refused under any other case's expectations.
        kill = SHADERS["t11_kill_frag_spirv"]
        with self.assertRaises(ValueError):
            checked_spirv(compiled["t11_terminate_frag_spirv"], *kill[2:])
        demote = SHADERS["t11_demote_frag_spirv"]
        with self.assertRaises(ValueError):
            checked_spirv(compiled["t11_terminate_frag_spirv"], *demote[2:])
        with self.assertRaises(ValueError):
            checked_spirv(compiled["t11_control_frag_spirv"], *kill[2:])
        words = list(struct.unpack(f"<{len(compiled['t11_kill_frag_spirv']) // 4}I",
                                   compiled["t11_kill_frag_spirv"]))
        words[1] = 0x00010300
        with self.assertRaisesRegex(ValueError, "version"):
            checked_spirv(struct.pack(f"<{len(words)}I", *words), *kill[2:])

    def test_payload_is_bounded_and_public(self):
        source = (ROOT / "examples/t11_kill_depth_witness/main.c").read_text()
        self.assertIn("fence_timeout = UINT64_C(300000000)", source)
        self.assertNotIn("vkQueueWaitIdle", source)
        # One readback per submission: the pass, then each aspect on its own.
        self.assertIn("SUBMISSIONS_PER_CASE = 3", source)
        self.assertEqual(source.count("copy_aspect(case_commands["), 2)
        self.assertNotIn('#include "vk_', source)
        self.assertIn("#include <ps5vk/ps5vk.h>", source)


if __name__ == "__main__":
    unittest.main()
