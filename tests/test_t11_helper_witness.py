"""Contracts for the T11 public-SDK helper-invocation witness builder and verifier."""

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
from build_t11_helper_witness import SHADERS  # noqa: E402
from build_t11_kill_depth_witness import checked_spirv, strip_extensions  # noqa: E402
from run_t11_helper_witness import (  # noqa: E402
    CASES, REMOVED, SUBMISSIONS_PER_CASE, verify)

EXACT = dict(kept_wrong=0, removed_written=0, written=64 * 64 - REMOVED)


def case_line(form, removes, kept_wrong=0, removed_written=0, written=None):
    if written is None:
        written = 64 * 64 - (REMOVED if removes else 0)
    return (f"T11_HELPER_WITNESS_CASE form={form} removes={removes} kept_wrong={kept_wrong} "
            f"removed_written={removed_written} written={written} px_0_0=00000000 "
            f"px_1_0=ff000101 px_0_1=ff000101 px_1_1=ff000101 digest=0badf00d "
            f"fence=complete")


def fixture_log(tallies=None, forms=CASES, steps=SUBMISSIONS_PER_CASE, retired=True,
                failure=False):
    tallies = tallies or {}
    lines = [f"T11_HELPER_WITNESS_START extent=64 cases={len(CASES)}"]
    lines += [f"T11_HELPER_WITNESS_PIPELINE form={form} created=1" for form in forms]
    for form in forms:
        lines += [f"T11_HELPER_WITNESS_STEP form={form} index={i} fence=complete"
                  for i in range(steps)]
        lines.append(case_line(form, int(form != "control"), **tallies.get(form, {})))
    lines.append(f"T11_HELPER_WITNESS_RESULT cases={len(forms)} "
                 f"submissions={len(forms) * steps} fence=complete")
    if failure:
        lines.append("T11_HELPER_WITNESS_FAILURE result=-3 step=x completed=1")
    if retired:
        lines.append("T11_HELPER_WITNESS_RETIRED resources=clean")
    return ("\n".join(lines) + "\n").encode()


def receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk", transport="tcp",
                clean=True, bye=True, gaps=[], sha256=hashlib.sha256(log).hexdigest(),
                run_id="unit-run")


def artifact():
    return dict(profile="t11-helper-public-sdk-witness", extent=64, format="R8G8B8A8_UNORM",
                cases=list(CASES), diagnostic_switch=None, eboot_sha256="artifact-sha")


class Verifier(unittest.TestCase):
    def test_live_helpers_pass(self):
        log = fixture_log()
        result = verify(log, receipt(log), artifact())
        self.assertTrue(result["helper_ok"])
        self.assertEqual(set(result["verdict"].values()), {"helpers-live"})

    def test_terminate_and_kill_are_reported_not_required(self):
        wrong = dict(kept_wrong=REMOVED * 3)
        log = fixture_log({"terminate": wrong, "kill": wrong})
        result = verify(log, receipt(log), artifact())
        self.assertTrue(result["helper_ok"])
        self.assertEqual(result["verdict"]["terminate"], "helper-derivatives-wrong")

    def test_demote_defects_fail_the_contract(self):
        for form in ("demote", "demote_dxvk", "demote_ext"):
            for tally, name in ((dict(kept_wrong=5), "helper-derivatives-wrong"),
                                (dict(removed_written=REMOVED), "removal-ignored")):
                with self.subTest(form=form, name=name):
                    log = fixture_log({form: tally})
                    result = verify(log, receipt(log), artifact())
                    self.assertFalse(result["helper_ok"])
                    self.assertEqual(result["verdict"][form], name)

    def test_invalid_runs_are_rejected(self):
        for log, art in (
                (fixture_log({"control": dict(kept_wrong=1)}), artifact()),
                (fixture_log({"control": dict(written=100)}), artifact()),
                (fixture_log(forms=CASES[:5]), artifact()),
                (fixture_log(steps=1), artifact()),
                (fixture_log(retired=False), artifact()),
                (fixture_log(failure=True), artifact()),
                (fixture_log(), dict(artifact(), diagnostic_switch={"X": "1"})),
                (fixture_log(), dict(artifact(), format="B8G8R8A8_UNORM"))):
            with self.subTest(art=art):
                with self.assertRaises(ValueError):
                    verify(log, receipt(log), art)


class Builder(unittest.TestCase):
    @unittest.skipUnless(shutil.which("glslangValidator"), "glslangValidator unavailable")
    def test_each_case_is_exactly_its_form(self):
        with tempfile.TemporaryDirectory() as directory:
            for name, (source, arguments, version, capabilities, removal, strip) in \
                    SHADERS.items():
                target = Path(directory) / f"{name}.spv"
                subprocess.run(["glslangValidator", "-V", *arguments, str(ROOT / source),
                                "-o", str(target)], check=True, capture_output=True)
                payload = target.read_bytes()
                if strip:
                    stripped = strip_extensions(payload)
                    self.assertLess(len(stripped), len(payload))
                    self.assertNotIn(b"SPV_EXT_demote", stripped)
                    payload = stripped
                with self.subTest(name=name):
                    checked_spirv(payload, version, capabilities, removal)
        with self.assertRaises(ValueError):
            strip_extensions(struct.pack("<5I", 0x07230203, 0x00010000, 0, 1, 0))

    def test_payload_is_bounded_and_public(self):
        source = (ROOT / "examples/t11_helper_witness/main.c").read_text()
        self.assertIn("fence_timeout = UINT64_C(300000000)", source)
        self.assertNotIn("vkQueueWaitIdle", source)
        self.assertNotIn('#include "vk_', source)
        self.assertIn("SUBMISSIONS_PER_CASE = 2", source)


if __name__ == "__main__":
    unittest.main()
