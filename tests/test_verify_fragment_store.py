"""Fail-closed mutation tests for the fragment-storage hardware verifier."""
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import verify_fragment_store as verifier  # noqa: E402


RECORDS = [
    "PS5VK_GRAPHICS_API_DEVICE_CREATED",
    "PS5VK_FRAGMENT_STORE_ABI set=0 binding=0 record_bytes=16 control_table=0 candidate_fragment_table=1 used_bindings=0000000000000001",
    "PS5VK_FRAGMENT_STORE_READBACK extent=64x64 draws=2 expected=4096 control_counter=0 candidate_counter=4096 guard_words=30 guard_mismatches=0 fence=success strict_verified=1",
    "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
    "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE",
]


class Fixture:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.artifact = root / "eboot.bin"
        self.artifact.write_bytes(b"fragment-store-probe")
        digest = hashlib.sha256(self.artifact.read_bytes()).hexdigest()
        self.manifest = root / "manifest.json"
        self.manifest.write_text(json.dumps({
            "stage": "graphics-api-offscreen-draw", "submit_enabled": True,
            "runtime_graphics": True, "termination": "return-main",
            "fragment_store_probe": 1, "t06_diagnostic_features": True,
            "graphics_shader_source": "owned-runtime-fragment-storage-atomic",
            "fragment_store_witness": verifier.EXPECTED_WITNESS,
            "files": {"eboot.bin": digest}}))
        self.run = root / "run"
        self.write(RECORDS)

    def write(self, records):
        lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=0x1234 tag=t06"]
        for sequence, message in enumerate(records, 1):
            lines.append(f"{sequence}\t{1000 + sequence}\tMARK\t{message}")
        lines.append(f"BYE seq={len(records)} reason=graphics-api-end")
        data = ("\n".join(lines) + "\n").encode()
        self.run.with_suffix(".log").write_bytes(data)
        self.run.with_suffix(".json").write_text(json.dumps({
            "protocol": "ps5log/1", "transport": "tcp", "clean": True,
            "bye": True, "gaps": [], "records": len(records),
            "sha256": hashlib.sha256(data).hexdigest(),
            "identity": {"title": "PPSA99994", "app": "ps5vk",
                         "boot": "0x1234"}}))

    def validate(self):
        return verifier.validate(self.run, self.manifest, self.artifact)


class FragmentStoreVerifierTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()

    def tearDown(self):
        self.fixture.tmp.cleanup()

    def test_accepts_exact_witness(self):
        result = self.fixture.validate()
        self.assertTrue(result["ok"])
        self.assertEqual(result["candidate_counter"], 4096)

    def test_rejects_every_material_mutation(self):
        mutations = {
            "abi": (1, "record_bytes=16", "record_bytes=32"),
            "control_table": (1, "control_table=0", "control_table=1"),
            "extent": (2, "extent=64x64", "extent=63x64"),
            "draws": (2, "draws=2", "draws=1"),
            "control": (2, "control_counter=0", "control_counter=1"),
            "candidate": (2, "candidate_counter=4096", "candidate_counter=4095"),
            "guard": (2, "guard_mismatches=0", "guard_mismatches=1"),
            "fence": (2, "fence=success", "fence=timeout"),
            "verified": (2, "strict_verified=1", "strict_verified=0"),
            "close": (3, "allocations_bytes=0", "allocations_bytes=64"),
        }
        for name, (index, old, new) in mutations.items():
            with self.subTest(name=name):
                records = list(RECORDS)
                records[index] = records[index].replace(old, new)
                self.fixture.write(records)
                self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_missing_duplicate_or_reordered_records(self):
        for records in (RECORDS[:-1], RECORDS + [RECORDS[-1]],
                        [RECORDS[1], RECORDS[0], *RECORDS[2:]]):
            self.fixture.write(records)
            self.assertRaises(ValueError, self.fixture.validate)

    def test_rejects_artifact_manifest_and_transport_drift(self):
        self.fixture.artifact.write_bytes(b"different")
        self.assertRaises(ValueError, self.fixture.validate)
        self.fixture.tmp.cleanup()
        self.fixture = Fixture()
        manifest = json.loads(self.fixture.manifest.read_text())
        manifest["fragment_store_witness"]["expected_fragments"] = 4095
        self.fixture.manifest.write_text(json.dumps(manifest))
        self.assertRaises(ValueError, self.fixture.validate)
        self.fixture.tmp.cleanup()
        self.fixture = Fixture()
        receipt = json.loads(self.fixture.run.with_suffix(".json").read_text())
        receipt["clean"] = False
        self.fixture.run.with_suffix(".json").write_text(json.dumps(receipt))
        self.assertRaises(ValueError, self.fixture.validate)


if __name__ == "__main__":
    unittest.main()
