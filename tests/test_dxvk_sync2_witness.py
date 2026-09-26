"""Contracts for the bounded public-SDK VK_KHR_synchronization2 witness."""

import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_sync2_witness import (  # noqa: E402
    PHASE_VALUES, PROFILE, SDK_SWITCHES, SEEDS, expected_digest)
from run_dxvk_sync2_witness import verify  # noqa: E402


def fixture_log(*, deferred=None, counters=PHASE_VALUES, mismatches=(0, 0, 0),
                buffers=(3, 1, 0), core="absent", swap=False, failure=False,
                retired=True):
    deferred = deferred or ("returned", str(PHASE_VALUES[0]), "timeout", "timeout", "yes")
    data = (SEEDS[0], SEEDS[1], SEEDS[1])
    results = [
        f"DXVK_SYNC2_WITNESS_RESULT phase={n + 1} buffers={buffers[n]} wait=success "
        f"counter={counters[n]} values=64 mismatches={mismatches[n]} "
        f"guard_mismatches=0 digest={expected_digest(data[n]):08x}" for n in range(3)]
    lines = [
        "DXVK_SYNC2_WITNESS_START sync2_spec=1 timeline_spec=2 commands=6 "
        f"core_names={core} timestamp_valid_bits=0 initial=1000 values=64",
        results[0],
        "DXVK_SYNC2_WITNESS_DEFERRED submit={} counter={} wait_zero={} "
        "wait_20ms={} untouched={}".format(*deferred),
        *(results[2:0:-1] if swap else results[1:]),
    ]
    if retired:
        lines.append("DXVK_SYNC2_WITNESS_RETIRED resources=clean")
    if failure:
        lines.append("DXVK_SYNC2_WITNESS_FAILURE call=x result=-1 retirement=attempted")
    return ("\n".join(lines) + "\n").encode()


def fixture_receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk",
                transport="tcp", clean=True, bye=True, gaps=[],
                sha256=hashlib.sha256(log).hexdigest(), run_id="unit-run")


def fixture_artifact(**changes):
    artifact = dict(profile=PROFILE, values=64, initial_value=1000, gate_value=1002,
                    phase_values=list(PHASE_VALUES), seeds=list(SEEDS),
                    sdk_switches=dict(SDK_SWITCHES), eboot_sha256="0" * 64)
    artifact.update(changes)
    return artifact


class Sync2WitnessContract(unittest.TestCase):
    def check(self, log, artifact=None):
        return verify(log, fixture_receipt(log), artifact or fixture_artifact())

    def test_strict_pass(self):
        result = self.check(fixture_log())
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["phase_values"], [1001, 1 << 40, (1 << 40) + 1])
        self.assertEqual(result["timestamp_valid_bits"], 0)

    def test_refusals(self):
        bad_logs = [
            fixture_log(deferred=("returned", "1001", "other", "timeout", "yes")),
            fixture_log(deferred=("returned", "1001", "timeout", "timeout", "no")),
            fixture_log(mismatches=(0, 1, 0)),
            fixture_log(counters=(1001, 1 << 40, 1 << 40)),
            fixture_log(buffers=(1, 1, 0)),
            fixture_log(core="present"),
            fixture_log(swap=True),
            fixture_log(failure=True),
            fixture_log(retired=False),
        ]
        for log in bad_logs:
            with self.subTest(log=log[-120:]):
                with self.assertRaises(ValueError):
                    self.check(log)

    def test_artifact_and_receipt_identity(self):
        log = fixture_log()
        with self.assertRaises(ValueError):
            self.check(log, fixture_artifact(sdk_switches={"PS5VK_SYNC2": "1"}))
        receipt = fixture_receipt(log)
        receipt["sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            verify(log, receipt, fixture_artifact())

    def test_route_is_shipping(self):
        sys.path.insert(0, str(ROOT / "tools"))
        from check_dxvk_profile import implemented_device_extensions
        self.assertIn("VK_KHR_synchronization2", implemented_device_extensions())
        self.assertEqual(SDK_SWITCHES, {})


if __name__ == "__main__":
    unittest.main()
