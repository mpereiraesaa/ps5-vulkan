"""Contracts for the public-SDK transform feedback witness and its switch."""

import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t14_xfb_witness import CASES, PROFILE, SWITCH, checked_spirv  # noqa: E402
from run_t14_xfb_witness import EXPECTED, verify  # noqa: E402


def artifact(**changes):
    value = {"profile": PROFILE, "diagnostic_switch": SWITCH, "cases": list(CASES),
             "eboot_sha256": "0" * 64}
    value.update(changes)
    return value


def case_line(name, *, ok=1, mismatches=0, counter0=None):
    points, want0, want1 = EXPECTED[name]
    counter0 = want0 if counter0 is None else counter0
    return (f"T14_XFB_WITNESS_CASE name={name} points={points} ok={ok} mismatches={mismatches}"
            f" first_bad={-1 if not mismatches else 7} sentinel_bad=0 before_bad=0"
            f" counter0={counter0} want0={want0} stream1_mismatches=0"
            f" stream1_sentinel_bad=0 counter1={want1} want1={want1}"
            f" w0=00000000,3f800000,00000000,40e00000"
            f" written={10 if name == 'query' else 0} needed={16 if name == 'query' else 0}"
            f" digest=1234abcd fence=complete")


def log(*, streams=4, override=None, retired=True, failure=False):
    lines = [f"T14_XFB_WITNESS_START feature=1 streams_feature=1 geometry=1 streams={streams}"
             " buffers=4 stride=2048 data=512 stream_data=512 queries=1 draw=1",
             "T14_XFB_WITNESS_PIPELINES created=2"]
    passed = 0
    for name in CASES:
        line = override.get(name) if override and name in override else case_line(name)
        passed += " ok=1 " in line
        lines.append(line)
    lines.append(f"T14_XFB_WITNESS_RESULT cases={len(CASES)} passed={passed}"
                 f" submissions={len(CASES)}")
    if failure:
        lines.append("T14_XFB_WITNESS_FAILURE result=-1 step=x completed=0")
    if retired:
        lines.append("T14_XFB_WITNESS_RETIRED resources=clean")
    return ("\n".join(lines) + "\n").encode()


def receipt(payload):
    return {"protocol": "ps5log/1", "title": "PPSA99994", "app": "ps5vk",
            "transport": "tcp", "clean": True, "bye": True, "gaps": [],
            "sha256": hashlib.sha256(payload).hexdigest(), "run_id": "fixture"}


class TransformFeedbackWitness(unittest.TestCase):
    def test_every_case_passing_is_strictly_verified(self):
        payload = log()
        result = verify(payload, receipt(payload), artifact())
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["cases"]["order"]["counter0"], 192000)
        self.assertEqual(result["cases"]["resume"]["counter0"], 224)

    def test_a_disordered_or_miscounted_case_is_not_verified(self):
        for override in ({"order": case_line("order", ok=0, mismatches=40)},
                         {"overflow": case_line("overflow", counter0=512)}):
            payload = log(override=override)
            self.assertFalse(verify(payload, receipt(payload), artifact())["strict_verified"])

    def test_reporting_failure_and_retirement_are_refused(self):
        for payload in (log(streams=1), log(retired=False), log(failure=True)):
            with self.assertRaises(ValueError):
                verify(payload, receipt(payload), artifact())
        payload = log()
        with self.assertRaises(ValueError):
            verify(payload, receipt(payload), artifact(diagnostic_switch="PS5VK_OTHER"))

    def test_switch_is_default_off_and_wired(self):
        platform = (ROOT / "native/platform_ps5.c").read_text()
        self.assertIn(f"#if defined({SWITCH}) && {SWITCH}", platform)
        self.assertIn(f'"{SWITCH}"', (ROOT / "tools/build_sdk.py").read_text())
        self.assertIn(f'"{SWITCH}"', (ROOT / "tools/check_dxvk_profile.py").read_text())

    def test_spirv_capability_screen(self):
        header = (0x07230203).to_bytes(4, "little") + (0x00010000).to_bytes(4, "little")
        body = b"\0" * 12
        capability = ((2 << 16) | 17).to_bytes(4, "little")
        checked_spirv(header + body + capability + (54).to_bytes(4, "little"))
        with self.assertRaises(ValueError):
            checked_spirv(header + body + capability + (61).to_bytes(4, "little"))


if __name__ == "__main__":
    unittest.main()
