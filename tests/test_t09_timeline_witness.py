"""Contracts for the isolated T09 public-SDK timeline-semaphore witness."""

import hashlib
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t09_timeline_witness import (  # noqa: E402
    PHASE_VALUES, PROFILE, SEEDS, checked_spirv, expected_digest)
from run_t09_timeline_witness import verify  # noqa: E402


def instruction(opcode, *operands):
    return [(len(operands) + 1) << 16 | opcode, *operands]


def fixture_spirv(*, extra_capability=None, model=(0, 1), version=0x00010000):
    words = [0x07230203, version, 0, 8, 0]
    words += instruction(17, 1)
    if extra_capability is not None:
        words += instruction(17, extra_capability)
    words += instruction(14, *model)
    return struct.pack(f"<{len(words)}I", *words)


def fixture_log(*, deferred=None, phase_values=PHASE_VALUES, mismatches=(0, 0),
                digests=None, retired=True, difference=18446744073709551615,
                swap=False, failure=False):
    digests = digests or [expected_digest(seed) for seed in SEEDS]
    deferred = deferred or ("returned", "1000", "timeout", "timeout", "yes")
    results = [
        f"T09_TIMELINE_WITNESS_RESULT phase={n + 1} wait=success "
        f"counter={phase_values[n]} values=64 mismatches={mismatches[n]} "
        f"guard_mismatches=0 digest={digests[n]:08x}" for n in range(2)]
    if swap:
        results.reverse()
    lines = [
        f"T09_TIMELINE_WITNESS_START spec=2 max_difference={difference} "
        "initial=1000 values=64",
        "T09_TIMELINE_WITNESS_DEFERRED submit={} counter={} wait_zero={} "
        "wait_20ms={} untouched={}".format(*deferred),
        *results,
    ]
    if retired:
        lines.append("T09_TIMELINE_WITNESS_RETIRED resources=clean")
    if failure:
        lines.append("T09_TIMELINE_WITNESS_FAILURE call=x result=-1 retirement=attempted")
    return ("\n".join(lines) + "\n").encode()


def fixture_receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk",
                transport="tcp", clean=True, bye=True, gaps=[],
                sha256=hashlib.sha256(log).hexdigest(), run_id="unit-run")


def fixture_artifact():
    return dict(profile=PROFILE, values=64, initial_value=1000, gate_value=1001,
                phase_values=list(PHASE_VALUES), seeds=list(SEEDS),
                eboot_sha256="0" * 64)


class SpirvContract(unittest.TestCase):
    def test_plain_vulkan10_compute_module(self):
        checked_spirv(fixture_spirv())

    def test_other_modules_rejected(self):
        for payload in (fixture_spirv(extra_capability=5347),
                        fixture_spirv(model=(5348, 1)),
                        fixture_spirv(version=0x00010300),
                        fixture_spirv()[:-1]):
            with self.subTest(size=len(payload)):
                with self.assertRaises(ValueError):
                    checked_spirv(payload)


class LogContract(unittest.TestCase):
    def test_exact_log_verifies(self):
        log = fixture_log()
        result = verify(log, fixture_receipt(log), fixture_artifact())
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["phase_values"], [1002, 1 << 40])
        self.assertEqual(result["max_timeline_semaphore_value_difference"],
                         18446744073709551615)
        self.assertEqual(result["run_id"], "unit-run")

    def test_premature_visibility_rejected(self):
        for deferred in (("returned", "1002", "timeout", "timeout", "yes"),
                         ("returned", "1000", "success", "timeout", "yes"),
                         ("returned", "1000", "timeout", "other", "yes"),
                         ("returned", "1000", "timeout", "timeout", "no")):
            with self.subTest(deferred=deferred):
                log = fixture_log(deferred=deferred)
                with self.assertRaises(ValueError):
                    verify(log, fixture_receipt(log), fixture_artifact())

    def test_data_counter_and_lifecycle_failures_rejected(self):
        wrong_digest = [expected_digest(SEEDS[0]) ^ 1, expected_digest(SEEDS[1])]
        for kwargs in (dict(mismatches=(1, 0)), dict(digests=wrong_digest),
                       dict(phase_values=(1002, 1 << 32)), dict(retired=False),
                       dict(difference=2147483646), dict(swap=True),
                       dict(failure=True)):
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
        artifact["seeds"] = [1, 2]
        with self.assertRaises(ValueError):
            verify(log, fixture_receipt(log), artifact)


if __name__ == "__main__":
    unittest.main()
