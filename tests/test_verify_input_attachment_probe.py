"""Fail-closed mutation tests for the input-attachment hardware verifier."""
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import verify_input_attachment_probe as verifier  # noqa: E402


RECORDS = [
    "PS5VK_INPUT_ATTACHMENT_QUERY format=37 usage=147 max_layers=256 max_width=16384 max_height=16384 samples=1",
    "PS5VK_INPUT_ATTACHMENT_RESOURCE create=success bind=success allocation_bytes=98304 layers=6 usage=147",
    "PS5VK_INPUT_ATTACHMENT_ABI set=0 binding=0 record_bytes=32 fragment_slot=1 fragment_count=2 used_bindings=0000000000000001 vertex_table=0",
    "PS5VK_SUBPASS_BOUNDARY serial=7 subpass=1 words=10",
    "PS5VK_GRAPHICS_COMPLETED serial=7 image_bytes=98304",
    "PS5VK_INPUT_ATTACHMENT_DRAW subpasses=2 draws=2 boundary=required dependency=colour-write-to-fragment-input-read-by-region",
    "PS5VK_GRAPHICS_COMPLETED serial=8 image_bytes=16384",
    "PS5VK_INPUT_ATTACHMENT_READBACK extent=64x64 layers=6 view_layer=0 matched=4096 total=4096 verdict=ok first_x=0 first_y=0 actual_hash=11223344 expected_hash=11223344 guard_words=4096 guard_mismatches=0 strict_verified=1",
    "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
    "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE",
]


class Fixture:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.artifact = root / "eboot.bin"
        self.artifact.write_bytes(b"input-attachment-probe")
        digest = hashlib.sha256(self.artifact.read_bytes()).hexdigest()
        self.manifest = root / "manifest.json"
        self.manifest.write_text(json.dumps({
            "stage": "graphics-api-offscreen-draw", "submit_enabled": True,
            "runtime_graphics": True, "termination": "return-main",
            "graphics_shader_source": "owned-runtime-input-attachment-oracle",
            "input_attachment_witness": verifier.EXPECTED_WITNESS,
            "files": {"eboot.bin": digest}}))
        self.run = root / "run"
        self.write(RECORDS)

    def write(self, records):
        lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=0x1234 tag=gate1"]
        for sequence, message in enumerate(records, 1):
            lines.append(f"{sequence}\t{1000 + sequence}\tMARK\t{message}")
        lines.append(f"BYE seq={len(records)} reason=graphics-api-end")
        data = ("\n".join(lines) + "\n").encode()
        self.run.with_suffix(".log").write_bytes(data)
        self.run.with_suffix(".json").write_text(json.dumps({
            "protocol": "ps5log/1", "transport": "tcp", "clean": True,
            "bye": True, "gaps": [], "records": len(records),
            "sha256": hashlib.sha256(data).hexdigest(),
            "identity": {"title": "PPSA99994", "app": "ps5vk", "boot": "0x1234"}}))

    def validate(self):
        return verifier.validate(self.run, self.manifest, self.artifact)


class TestInputAttachmentProbeVerifier(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()

    def tearDown(self):
        self.fixture.tmp.cleanup()

    def test_accepts_faithful_run(self):
        result = self.fixture.validate()
        self.assertTrue(result["ok"])
        self.assertEqual(result["pixels"], 4096)

    def test_rejects_each_material_contract_mutation(self):
        mutations = {
            "query": (0, "usage=147", "usage=19"),
            "resource": (1, "layers=6", "layers=1"),
            "abi": (2, "record_bytes=32", "record_bytes=48"),
            "boundary": (3, "subpass=1", "subpass=0"),
            "draw": (5, "draws=2", "draws=1"),
            "pixels": (7, "matched=4096", "matched=4095"),
            "hash": (7, "actual_hash=11223344", "actual_hash=55667788"),
            "guard": (7, "guard_mismatches=0", "guard_mismatches=1"),
            "close": (8, "allocations_bytes=0", "allocations_bytes=64"),
        }
        for name, (index, old, new) in mutations.items():
            with self.subTest(name=name):
                records = list(RECORDS)
                records[index] = records[index].replace(old, new)
                self.fixture.write(records)
                self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_missing_or_duplicate_required_record(self):
        for records in (RECORDS[:-1], RECORDS + [RECORDS[-1]]):
            self.fixture.write(records)
            self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_artifact_mismatch(self):
        self.fixture.artifact.write_bytes(b"different")
        self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_runtime_error_and_unclean_transport(self):
        log = self.fixture.run.with_suffix(".log")
        data = log.read_text().replace("\tMARK\tPS5VK_INPUT_ATTACHMENT_DRAW",
                                       "\tERR\tPS5VK_INPUT_ATTACHMENT_DRAW").encode()
        log.write_bytes(data)
        receipt_path = self.fixture.run.with_suffix(".json")
        receipt = json.loads(receipt_path.read_text())
        receipt["sha256"] = hashlib.sha256(data).hexdigest()
        receipt_path.write_text(json.dumps(receipt))
        self.assertRaises(ValueError, self.fixture.validate)


if __name__ == "__main__":
    unittest.main()
