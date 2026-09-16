"""Host regression for the clip/cull witness verifier.

The verifier is the only thing standing between a witness log and a promotion
claim, so every way a log can be wrong is exercised here: a missing case, a
coverage count that does not match the geometry, a pre-raster state that does
not match the case, and the digest relations that a collapsed or stale readback
would break.
"""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


verify = load_tool("verify_clip_cull")


class Fixture:
    def __init__(self):
        self.records = ["PS5VK_BOOT stage=graphics-api submit_enabled=1"]
        digests = {0: "1111111111111111", 1: "1111111111111111",
                   2: "2222222222222222", 3: "3333333333333333",
                   4: "4444444444444444", 5: "4444444444444444",
                   6: "3333333333333333"}
        for case, mode, expected in verify.CASES:
            state = verify.VS_OUT_CONFIG[-1 if mode < 0 else 0]
            pos = verify.POS_FORMAT[-1 if mode < 0 else 0]
            cntl = verify.VS_OUT_CNTL[-1 if mode < 0 else 0]
            self.records.append(
                f"PS5VK_CLIP_CULL_DRAW case={case} mode={mode} vs_out_config={state} "
                f"pos_format={pos} vs_out_cntl={cntl} vertices=6 instances=1")
            self.records.append(
                f"PS5VK_CLIP_CULL_CASE case={case} mode={mode} pixels={verify.PIXELS} "
                f"expected={expected} covered={expected} missing=0 foreign=0 wrong_color=0 "
                f"digest={digests[case]} verified=1")
        self.digests = digests
        self.records.append(self.summary())
        self.records.append("PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")
        self.log, self.receipt = self.encode(self.records)
        self.artifact = {
            "title": "PPSA99994", "stage": "graphics-api-offscreen-draw",
            "compiler": "runtime-psbc-aco", "target_gfx": 1013,
            "graphics_shader_source": "owned-runtime-clip-cull-distances",
            "geometry_fixture": "clip-cull-distance-coverage",
            "clip_cull_probe": 1, "clip_cull_extent": verify.EXTENT,
            "clip_cull_cases": len(verify.CASES), "sample_count": 1, "scene": None,
            "termination": "shell-close-after-cleanup", "submit_enabled": True,
            "files": {"eboot.bin": "ab" * 32},
        }

    def summary(self):
        fields = [f"cases={len(verify.CASES)}", f"extent={verify.EXTENT}",
                  "clear=000000ff", "clip_mask=03", "cull_mask=0c",
                  "control_mask=000000"]
        fields += [f"{name}={self.digests[case]}"
                   for case, name in verify.DIGEST_NAMES.items()]
        fields.append("strict_verified=1")
        return "PS5VK_CLIP_CULL_PROBE " + " ".join(fields)

    @staticmethod
    def encode(records):
        lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=0x1234 tag=graphics"]
        lines += [f"{seq}\t{1000 + seq}\tMARK\t{record}"
                  for seq, record in enumerate(records, 1)]
        lines.append(f"BYE seq={len(records)} reason=graphics-api-end")
        data = ("\n".join(lines) + "\n").encode()
        receipt = {"protocol": "ps5log/1", "transport": "tcp", "clean": True,
                   "bye": True, "gaps": [], "raw_lines": 0,
                   "sha256": hashlib.sha256(data).hexdigest()}
        return data, receipt

    def rebuild(self, records):
        self.log, self.receipt = self.encode(records)

    def validate(self):
        return verify.validate(self.log, self.receipt, self.artifact)


class ClipCullVerifierTests(unittest.TestCase):
    def test_complete_witness_is_accepted(self):
        fixture = Fixture()
        result = fixture.validate()
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["cases"], len(verify.CASES))
        self.assertFalse(result["deployment_identity_verified"])

    def test_missing_or_duplicated_case_is_rejected(self):
        fixture = Fixture()
        records = [r for r in fixture.records if "case=3 " not in r]
        fixture.rebuild(records)
        with self.assertRaises(ValueError):
            fixture.validate()
        fixture = Fixture()
        fixture.rebuild(fixture.records + [fixture.records[1]])
        with self.assertRaises(ValueError):
            fixture.validate()

    def test_coverage_must_match_the_geometry(self):
        import re
        for key, value in (("expected", "1000"), ("covered", "1000"),
                           ("missing", "1"), ("foreign", "7"),
                           ("wrong_color", "2"), ("verified", "0"),
                           ("pixels", "1024")):
            fixture = Fixture()
            records = []
            for record in fixture.records:
                if record.startswith("PS5VK_CLIP_CULL_CASE") and "case=4 " in record:
                    record = re.sub(rf"\b{key}=\S+", f"{key}={value}", record)
                records.append(record)
            fixture.rebuild(records)
            with self.assertRaises(ValueError, msg=f"{key}={value}"):
                fixture.validate()

    def test_pre_raster_state_must_match_the_case(self):
        fixture = Fixture()
        records = []
        for record in fixture.records:
            if record.startswith("PS5VK_CLIP_CULL_DRAW case=2"):
                record = record.replace("pos_format=00000044", "pos_format=00000004")
            records.append(record)
        fixture.rebuild(records)
        with self.assertRaises(ValueError):
            fixture.validate()

    def test_digest_relations_are_required(self):
        # A collapsed readback: every case hashes the same.
        fixture = Fixture()
        records = []
        for record in fixture.records:
            if record.startswith("PS5VK_CLIP_CULL_CASE"):
                import re
                record = re.sub(r"digest=\S+", "digest=9999999999999999", record)
            if record.startswith("PS5VK_CLIP_CULL_PROBE"):
                import re
                record = re.sub(r"digest_\w+=\S+", "digest_x=9999999999999999", record)
            records.append(record)
        fixture.rebuild(records)
        with self.assertRaises(ValueError):
            fixture.validate()
        # A stale summary digest that no case produced.
        fixture = Fixture()
        records = [r.replace("digest_plain=1111111111111111",
                             "digest_plain=0000000000000000")
                   for r in fixture.records]
        fixture.rebuild(records)
        with self.assertRaises(ValueError):
            fixture.validate()

    def test_receipt_and_artifact_identity_are_required(self):
        fixture = Fixture()
        receipt = dict(fixture.receipt, clean=False)
        with self.assertRaises(ValueError):
            verify.validate(fixture.log, receipt, fixture.artifact)
        artifact = copy.deepcopy(fixture.artifact)
        artifact["clip_cull_probe"] = 0
        with self.assertRaises(ValueError):
            verify.validate(fixture.log, fixture.receipt, artifact)
        artifact = copy.deepcopy(fixture.artifact)
        artifact["files"]["eboot.bin"] = "short"
        with self.assertRaises(ValueError):
            verify.validate(fixture.log, fixture.receipt, artifact)
        with self.assertRaises(ValueError):
            verify.validate(fixture.log + b"tampered\n", fixture.receipt, fixture.artifact)

    def test_cli_writes_the_verdict(self):
        fixture = Fixture()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "run.log").write_bytes(fixture.log)
            (root / "run.json").write_text(json.dumps(fixture.receipt))
            (root / "artifact.json").write_text(json.dumps(fixture.artifact))
            import subprocess
            import sys
            done = subprocess.run(
                [sys.executable, str(ROOT / "tools/verify_clip_cull.py"),
                 str(root / "run.log"), str(root / "run.json"),
                 str(root / "artifact.json")],
                capture_output=True, text=True, check=True)
            verdict = json.loads(done.stdout)
            self.assertTrue(verdict["strict_verified"])


if __name__ == "__main__":
    unittest.main()
