"""Mutation tests for the occlusion-probe verifier.

The verifier is the only thing standing between "the payload printed numbers"
and "the console produced a counter", so it gets its own negative fixtures:
every mutation below must make it fail. The synthetic log is otherwise a
faithful copy of the shape the probe really emits.
"""
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import verify_occlusion_probe as verifier  # noqa: E402

AVAILABILITY = 1 << 63


def build_records(available=16, counter=None, indices=None, first_pair=0,
                  begin_bit=True, deltas=None):
    """Return the probe record bodies for a synthetic run."""
    indices = indices if indices is not None else list(range(available))
    deltas = deltas if deltas is not None else [100 + i for i in indices]
    records = []
    base = 0x208980000
    records.append(("PS5VK_OCCLUSION_PROBE_BEGIN", f"serial=7 base={base:x} pairs=64"))
    records.append(("PS5VK_OCCLUSION_PROBE_END", f"serial=7 base_plus_8={base + 8:x}"))
    records.append(("PS5VK_GRAPHICS_COMPLETED", f"serial=7 image_bytes=0"))
    total = 0
    for index, delta in zip(indices, deltas):
        start = 0 if begin_bit else 5
        end = start + delta
        total += delta
        begin_word = (AVAILABILITY | start) if begin_bit else start
        end_word = (AVAILABILITY | end) if begin_bit else end
        records.append(("PS5VK_OCCLUSION_PROBE_PAIR",
                        f"serial=7 index={index} begin={begin_word:016x} "
                        f"end={end_word:016x} delta={delta}"))
    if counter is None:
        counter = total
    mask = sum(1 << i for i in indices)
    records.append(("PS5VK_OCCLUSION_PROBE_SLOT",
                    f"serial=7 pairs=64 available={len(indices)} "
                    f"first_pair={indices[0] if indices else first_pair} "
                    f"highest_pair={indices[-1] if indices else first_pair} "
                    f"mask_lo={mask & 0xffffffff:08x} mask_hi={mask >> 32:08x} "
                    f"counter={counter}"))
    records.append(("PS5VK_PLATFORM_CLOSE", "rc=0 allocations_bytes=0"))
    return records


class ProbeFixture:
    def __init__(self, records):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.artifact = root / "eboot.bin"
        self.artifact.write_bytes(b"occlusion-probe-artifact")
        digest = hashlib.sha256(self.artifact.read_bytes()).hexdigest()
        self.manifest = root / "manifest.json"
        self.manifest.write_text(json.dumps({
            "stage": "graphics-api-native-presentation-reuse",
            "submit_enabled": True,
            "scissor_probe": 15,
            "termination": "shell-close-after-cleanup",
            "files": {"eboot.bin": digest},
        }))
        self.run = root / "20260915T000000000Z_PPSA99994_ps5vk_0xdeadbeef"
        body = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=0xdeadbeef tag=gate1"]
        for seq, (kind, fields) in enumerate(records, 1):
            body.append(f"{seq}\t{1000 + seq}\tMARK\t{kind} {fields}")
        body.append(f"BYE seq={len(records)} reason=graphics-api-end")
        self.log_text = "\n".join(body) + "\n"
        self.log_path = self.run.with_suffix(".log")
        self.log_path.write_text(self.log_text)
        self.receipt = {
            "schema": 1, "protocol": "ps5log/1", "transport": "tcp",
            "clean": True, "bye": True, "gaps": [], "records": len(records),
            "sha256": hashlib.sha256(self.log_path.read_bytes()).hexdigest(),
            "identity": {"title": "PPSA99994", "app": "ps5vk",
                         "boot": "0xdeadbeef"},
        }
        self.write_receipt()

    def write_receipt(self):
        self.run.with_suffix(".json").write_text(json.dumps(self.receipt))

    def rewrite_log(self, text):
        self.log_path.write_text(text)
        self.receipt["sha256"] = hashlib.sha256(self.log_path.read_bytes()).hexdigest()
        self.write_receipt()

    def validate(self):
        return verifier.validate(self.run, self.manifest, self.artifact)


class TestOcclusionProbeVerifier(unittest.TestCase):
    def setUp(self):
        self.fixture = ProbeFixture(build_records())

    def tearDown(self):
        self.fixture.tmp.cleanup()

    def replace_fixture(self, records):
        self.fixture.tmp.cleanup()
        self.fixture = ProbeFixture(records)

    def test_accepts_the_faithful_run(self):
        result = self.fixture.validate()
        self.assertTrue(result["ok"])
        self.assertEqual(result["geometry"]["enabled_render_backends"], 16)
        self.assertEqual(result["geometry"]["mask"], "0x000000000000ffff")
        self.assertEqual(result["geometry"]["counter"], sum(100 + i for i in range(16)))

    def test_rejects_wrong_manifest_stage(self):
        manifest = json.loads(self.fixture.manifest.read_text())
        manifest["stage"] = "graphics-api-native-runtime"
        self.fixture.manifest.write_text(json.dumps(manifest))
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_wrong_scissor_probe(self):
        manifest = json.loads(self.fixture.manifest.read_text())
        manifest["scissor_probe"] = 14
        self.fixture.manifest.write_text(json.dumps(manifest))
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_artifact_that_does_not_match_the_manifest(self):
        manifest = json.loads(self.fixture.manifest.read_text())
        manifest["files"]["eboot.bin"] = "0" * 64
        self.fixture.manifest.write_text(json.dumps(manifest))
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_hello_identity_mismatch(self):
        self.fixture.rewrite_log(self.fixture.log_text.replace("boot=0xdeadbeef",
                                                              "boot=0xcafef00d"))
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_serial_mismatch(self):
        self.fixture.rewrite_log(
            self.fixture.log_text.replace("PS5VK_OCCLUSION_PROBE_SLOT serial=7",
                                          "PS5VK_OCCLUSION_PROBE_SLOT serial=9"))
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_missing_completion_for_the_serial(self):
        self.fixture.rewrite_log(self.fixture.log_text.replace(
            "PS5VK_GRAPHICS_COMPLETED serial=7 image_bytes=0",
            "PS5VK_GRAPHICS_COMPLETED serial=8 image_bytes=0"))
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_pair_rows_that_are_not_contiguous(self):
        self.replace_fixture(build_records(indices=[0, 1, 3, 4]))
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_mask_that_does_not_match_the_indices(self):
        text = self.fixture.log_text.replace("mask_lo=0000ffff", "mask_lo=0000fffe")
        self.fixture.rewrite_log(text)
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_availability_bit_missing(self):
        self.replace_fixture(build_records(begin_bit=False))
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_delta_that_does_not_match_the_words(self):
        text = self.fixture.log_text.replace("delta=100", "delta=999", 1)
        self.fixture.rewrite_log(text)
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_counter_that_is_not_the_pair_sum(self):
        self.replace_fixture(build_records(counter=1))
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_zero_available_pairs(self):
        self.replace_fixture(build_records(available=0, indices=[]))
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_unclean_close(self):
        text = self.fixture.log_text.replace(
            "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
            "PS5VK_PLATFORM_CLOSE rc=-1 allocations_bytes=64")
        self.fixture.rewrite_log(text)
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_a_second_close(self):
        text = self.fixture.log_text.replace(
            "BYE seq=", "39\t1012\tMARK\tPS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0\nBYE seq=")
        self.fixture.rewrite_log(text)
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_out_of_order_pair_rows(self):
        lines = self.fixture.log_text.splitlines()
        pair_first = next(i for i, line in enumerate(lines)
                          if "OCCLUSION_PROBE_PAIR" in line and "index=0 " in line)
        pair_second = next(i for i, line in enumerate(lines)
                           if "OCCLUSION_PROBE_PAIR" in line and "index=1 " in line)
        lines[pair_first], lines[pair_second] = lines[pair_second], lines[pair_first]
        self.fixture.rewrite_log("\n".join(lines) + "\n")
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_missing_bye(self):
        self.fixture.rewrite_log(self.fixture.log_text.replace(
            "BYE seq=21 reason=graphics-api-end", "").rstrip() + "\n")
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_error_records(self):
        self.fixture.rewrite_log(self.fixture.log_text.replace(
            "\tMARK\tPS5VK_GRAPHICS_COMPLETED serial=7",
            "\tERR\tPS5VK_GRAPHICS_COMPLETED serial=7"))
        self.assertRaises(ValueError, self.fixture.validate)

    def test_two_run_comparison_requires_identical_geometry(self):
        other = ProbeFixture(build_records(counter=2, deltas=[1] * 16))
        try:
            self.assertRaises(ValueError, verifier.compare, self.fixture.run,
                              other.run, self.fixture.manifest, self.fixture.artifact)
        finally:
            other.tmp.cleanup()


if __name__ == "__main__":
    unittest.main()
