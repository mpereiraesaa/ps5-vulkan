"""Contracts for the public-SDK 1 GiB device-memory witness and its budget."""

import hashlib
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_dxvk_heap_witness import (  # noqa: E402
    GIB, HEADROOM_CHUNK_BYTES, ITERATIONS, PROFILE, REGION_BYTES, REGION_OFFSETS,
    expected_digest, seed_for)
from run_dxvk_heap_witness import HEAP_BYTES, verify  # noqa: E402


def fixture_log(*, heap=HEAP_BYTES, boundary=-2, fences=(0, 0, 0), mismatch=None,
                chunks=7, refusal=-2, kernel_failure=False, retired=True,
                stages=("boot", "device", "held", "full", "released", "released",
                        "released", "closed")):
    direct = {stage: f"DXVK_HEAP_WITNESS_DIRECT_MEMORY stage={stage} "
              "capacity=13421772800 rc=0 block_start=0 block_bytes=4294967296"
              for stage in set(stages)}
    lines = [direct[stages[0]], direct[stages[1]],
             f"DXVK_HEAP_WITNESS_START heap={heap} heaps=1 types=1 "
             "storage_range=268435456 allocations=2048 ssbo_align=256 atom=64"]
    rest = list(stages[2:])
    for n in range(ITERATIONS):
        if n == 0:
            lines.append(f"DXVK_HEAP_WITNESS_BOUNDARY bytes={GIB + 131072} "
                         f"result={boundary}")
        lines.append(f"DXVK_HEAP_WITNESS_FENCE iteration={n} result={fences[n]}")
        for r, offset in enumerate(REGION_OFFSETS):
            bad = 1 if mismatch == (n, r) else 0
            lines.append(
                f"DXVK_HEAP_WITNESS_RESULT iteration={n} region={r} "
                f"offset={offset} mismatches={bad} guard_mismatches=0 "
                f"digest={expected_digest(seed_for(n, r)):08x}")
        if n == 0:
            lines += [direct[rest.pop(0)], direct[rest.pop(0)],
                      f"DXVK_HEAP_WITNESS_HEADROOM chunk={HEADROOM_CHUNK_BYTES} "
                      f"chunks={chunks} bytes={chunks * HEADROOM_CHUNK_BYTES} "
                      f"refusal={refusal}"]
        if rest:
            lines.append(direct[rest.pop(0)])
    lines += [direct[stage] for stage in rest]
    if kernel_failure:
        lines.append("PS5VK_MEMORY_ALLOC_FAILED bytes=33554432 rc=-1")
    if retired:
        lines.append("DXVK_HEAP_WITNESS_RETIRED resources=clean")
    return ("\n".join(lines) + "\n").encode()


def fixture_receipt(log):
    return dict(protocol="ps5log/1", title="PPSA99994", app="ps5vk",
                transport="tcp", clean=True, bye=True, gaps=[],
                sha256=hashlib.sha256(log).hexdigest(), run_id="unit-run")


def fixture_artifact():
    return dict(profile=PROFILE, iterations=ITERATIONS, region_bytes=REGION_BYTES,
                region_offsets=list(REGION_OFFSETS),
                headroom_chunk_bytes=HEADROOM_CHUNK_BYTES, eboot_sha256="0" * 64)


class BudgetContract(unittest.TestCase):
    def test_windows_cover_first_middle_last_64k(self):
        self.assertEqual(REGION_OFFSETS, (0, 1 << 29, (1 << 30) - 65536))
        self.assertEqual(GIB, 1 << 30)

    def test_verifier_heap_matches_the_graphics_profile(self):
        header = (ROOT / "src/device_profile_report.h").read_text()
        self.assertRegex(header, r"PS5VK_PROFILE_GRAPHICS_MAX_ALLOCATION_BYTES "
                         r"\(UINT64_C\(1\) << 30\)")
        self.assertIn("(PS5VK_PROFILE_GRAPHICS_MAX_ALLOCATION_BYTES + "
                      "UINT64_C(256) * 1024 * 1024)", header)
        self.assertEqual(HEAP_BYTES, (1 << 30) + (256 << 20))

    def test_witness_waits_only_on_bounded_fences(self):
        source = (ROOT / "examples/dxvk_heap_witness/main.c").read_text()
        self.assertNotRegex(source, r"vkQueueWaitIdle|vkDeviceWaitIdle|UINT64_MAX")
        self.assertIn("#define FENCE_NS UINT64_C(300000000)", source)
        self.assertEqual(len(re.findall(r"vkWaitForFences\(", source)), 1)


class LogContract(unittest.TestCase):
    def test_exact_log_verifies(self):
        log = fixture_log()
        result = verify(log, fixture_receipt(log), fixture_artifact())
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["heap_bytes"], HEAP_BYTES)
        self.assertEqual(result["headroom_bytes"], 7 * HEADROOM_CHUNK_BYTES)
        self.assertEqual(result["direct_memory"]["boot"][0]["capacity"], 13421772800)
        self.assertEqual(len(result["direct_memory"]["released"]), 3)

    def test_failures_rejected(self):
        for kwargs in (dict(heap=256 << 20), dict(boundary=0),
                       dict(fences=(0, 2, 0)), dict(mismatch=(2, 2)),
                       dict(chunks=3), dict(chunks=9), dict(refusal=0),
                       dict(kernel_failure=True), dict(retired=False),
                       dict(stages=("boot", "device", "held", "full", "released",
                                    "released", "closed", "closed"))):
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
        artifact["region_offsets"] = [0]
        with self.assertRaises(ValueError):
            verify(log, fixture_receipt(log), artifact)


if __name__ == "__main__":
    unittest.main()
