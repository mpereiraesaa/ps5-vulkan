"""Host regression for the geometry witness verifier.

The verifier is the only thing standing between a witness log and a promotion
claim, so every way a log can be wrong is exercised here: a missing case, a
coverage count that does not match the geometry, a geometry case without its
pipeline state, and the digest relations a collapsed or stale readback breaks.
"""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


verify = load_tool("verify_geometry")


class Fixture:
    def __init__(self):
        # Exactly the relations the verifier requires: passthrough reproduces the
        # control image and the other three cases all differ from it.
        self.digests = {0: "1111111111111111", 1: "1111111111111111",
                        2: "2222222222222222", 3: "3333333333333333",
                        4: "4444444444444444", 5: "1111111111111111",
                        6: "6666666666666666", 7: "7777777777777777",
                        # The sentinel shares the control's coverage and differs
                        # from it in the colour its own mapping computes.
                        8: "8888888888888888",
                        # The indexed marker draws two small quads, so it differs
                        # from every full-coverage case by construction.
                        9: "9999999999999999",
                        # The readback draws four small squares carrying the raw
                        # bytes of the value the stage read, so it differs from
                        # every other case as well.
                        10: "aaaaaaaaaaaaaaaa",
                        # The other two readbacks read the other two input
                        # vertices, so they are three different images.
                        11: "bbbbbbbbbbbbbbbb",
                        12: "cccccccccccccccc"}
        records = ["PS5VK_BOOT stage=graphics-api submit_enabled=1"]
        for case, mode, expected in verify.CASES:
            records.append(
                f"PS5VK_GEOMETRY_DRAW case={case} mode={mode} stages="
                f"{2 if mode < 0 else 3} out_prim_type={0 if mode < 0 else 2} "
                f"max_vertices={0 if mode < 0 else 9} "
                f"vertices={verify.DRAW_VERTICES.get(case, 6)} instances=1")
            records.append(
                f"PS5VK_GEOMETRY_CASE case={case} mode={mode} pixels={verify.PIXELS} "
                f"expected={expected} covered={expected} missing=0 foreign=0 wrong_color=0 "
                f"digest={self.digests[case]} verified=1")
        records.append(self.summary())
        records.append("PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")
        self.records = records
        self.log, self.receipt = self.encode(records)
        self.artifact = {
            "title": "PPSA99994", "stage": "graphics-api-offscreen-draw",
            "compiler": "runtime-psbc-aco", "target_gfx": 1013,
            "graphics_shader_source": "owned-runtime-geometry-stage",
            "geometry_fixture": "geometry-stage-coverage",
            "geometry_probe": 1, "geometry_extent": verify.EXTENT,
            "geometry_cases": len(verify.CASES), "sample_count": 1, "scene": None,
            "termination": "shell-close-after-cleanup", "submit_enabled": True,
            "files": {"eboot.bin": "ab" * 32},
        }

    def summary(self):
        fields = [f"cases={len(verify.CASES)}", f"extent={verify.EXTENT}",
                  "clear=000000ff", "out_prim_type=2", "max_vertices=9"]
        fields += [f"{name}={self.digests[case]}"
                   for case, name in verify.DIGEST_NAMES.items()]
        fields.append("strict_verified=1")
        return "PS5VK_GEOMETRY_PROBE " + " ".join(fields)

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
        self.records = records
        self.log, self.receipt = self.encode(records)

    def validate(self):
        return verify.validate(self.log, self.receipt, self.artifact)


class GeometryVerifierTests(unittest.TestCase):
    def test_complete_witness_is_accepted(self):
        result = Fixture().validate()
        self.assertTrue(result["strict_verified"])
        self.assertEqual(result["cases"], len(verify.CASES))
        self.assertFalse(result["deployment_identity_verified"])

    def test_missing_or_duplicated_case_is_rejected(self):
        fixture = Fixture()
        fixture.rebuild([r for r in fixture.records if "case=3 " not in r])
        with self.assertRaises(ValueError):
            fixture.validate()
        fixture = Fixture()
        fixture.rebuild(fixture.records + [fixture.records[1]])
        with self.assertRaises(ValueError):
            fixture.validate()

    def test_coverage_must_match_the_geometry(self):
        for key, value in (("expected", "1000"), ("covered", "1000"),
                           ("missing", "1"), ("foreign", "7"),
                           ("wrong_color", "2"), ("verified", "0"),
                           ("pixels", "1024")):
            fixture = Fixture()
            fixture.rebuild([
                re.sub(rf"\b{key}=\S+", f"{key}={value}", record)
                if record.startswith("PS5VK_GEOMETRY_CASE") and "case=2 " in record
                else record for record in fixture.records])
            with self.assertRaises(ValueError, msg=f"{key}={value}"):
                fixture.validate()

    def test_geometry_case_needs_its_pipeline_state(self):
        for key, value in (("stages", "2"), ("out_prim_type", "1"),
                           ("max_vertices", "3")):
            fixture = Fixture()
            fixture.rebuild([
                re.sub(rf"\b{key}=\S+", f"{key}={value}", record)
                if record.startswith("PS5VK_GEOMETRY_DRAW") and "case=1 " in record
                else record for record in fixture.records])
            with self.assertRaises(ValueError, msg=f"{key}={value}"):
                fixture.validate()

    def test_digest_relations_are_required(self):
        # The amplified image must reproduce the control image too.
        fixture = Fixture()
        fixture.rebuild([r.replace("digest=1111111111111111", "digest=9999999999999999")
                         if r.startswith("PS5VK_GEOMETRY_CASE") and "case=5 " in r
                         else r for r in fixture.records])
        with self.assertRaises(ValueError):
            fixture.validate()
        # Passthrough must reproduce the control image.
        fixture = Fixture()
        fixture.rebuild([r.replace("digest=1111111111111111", "digest=9999999999999999")
                         if r.startswith("PS5VK_GEOMETRY_CASE") and "case=1 " in r
                         else r for r in fixture.records])
        with self.assertRaises(ValueError):
            fixture.validate()
        # A collapsed readback: every case hashes the same.
        fixture = Fixture()
        fixture.rebuild([re.sub(r"digest=\S+", "digest=9999999999999999", r)
                         if r.startswith("PS5VK_GEOMETRY_CASE") else
                         re.sub(r"digest_\w+=\S+", "digest_x=9999999999999999", r)
                         if r.startswith("PS5VK_GEOMETRY_PROBE") else r
                         for r in fixture.records])
        with self.assertRaises(ValueError):
            fixture.validate()
        # A stale summary digest no case produced.
        fixture = Fixture()
        fixture.rebuild([r.replace("digest_suppress=3333333333333333",
                                   "digest_suppress=0000000000000000")
                         for r in fixture.records])
        with self.assertRaises(ValueError):
            fixture.validate()
        # The sentinel must not reproduce the control image: that image carries
        # the control's blue, so a sentinel that hashed equal to it is a draw
        # whose colour did not come from the read the case asserts.
        fixture = Fixture()
        fixture.rebuild([r.replace("digest=8888888888888888", "digest=1111111111111111")
                         if r.startswith("PS5VK_GEOMETRY_CASE") and "case=8 " in r
                         else r.replace("digest_sentinel=8888888888888888",
                                        "digest_sentinel=1111111111111111")
                         if r.startswith("PS5VK_GEOMETRY_PROBE") else r
                         for r in fixture.records])
        with self.assertRaises(ValueError):
            fixture.validate()

    def test_case_count_agrees_with_the_header(self):
        """The certified table and the header's case set are one number in two
        places: a disagreement would let a witness run certify a matrix the
        binary did not draw."""
        header = (ROOT / "src/geometry_witness.h").read_text()
        total = int(re.search(r"PS5VK_GEOMETRY_CASES = (\d+)", header).group(1))
        self.assertEqual(len(verify.CASES), total)
        # The manifest the builder advertises has to carry the same number: the
        # parser refuses an artifact whose case count disagrees with the table it
        # certified, and a builder left behind would refuse every real run.
        builder = (ROOT / "tools/build_native.py").read_text()
        advertised = set(re.findall(r"geometry_cases=(\d+)", builder))
        self.assertEqual(advertised, {str(total)})
        # The sentinel is part of the certified matrix: it is the case whose
        # verdict asserts the value the geometry stage read.
        self.assertIn(8, {case for case, _, _ in verify.CASES})
        self.assertIn(8, verify.DIGEST_NAMES)
        # The raw readback is the case that reports the bits the geometry half
        # read, so it has to be in the certified matrix too.
        self.assertIn(10, {case for case, _, _ in verify.CASES})
        self.assertIn(10, verify.DIGEST_NAMES)
        # The readback's full-wave draw is one number in two places: the parser
        # certifies the vertex count the binary actually drew, and the native
        # case table has to draw it.
        native = (ROOT / "native/graphics_main.c").read_text()
        declared = int(
            re.search(r"PS5VK_GEOMETRY_READ_VERTICES = (\d+)", native).group(1))
        self.assertEqual(declared, verify.READ_VERTICES)
        self.assertEqual(set(verify.DRAW_VERTICES.values()), {declared})

    def test_receipt_and_artifact_identity_are_required(self):
        fixture = Fixture()
        with self.assertRaises(ValueError):
            verify.validate(fixture.log, dict(fixture.receipt, clean=False), fixture.artifact)
        for field, value in (("geometry_probe", 0),
                             ("graphics_shader_source", "owned-runtime-triangle"),
                             ("geometry_cases", 5)):
            artifact = copy.deepcopy(fixture.artifact)
            artifact[field] = value
            with self.assertRaises(ValueError, msg=field):
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
            done = subprocess.run(
                [sys.executable, str(ROOT / "tools/verify_geometry.py"),
                 str(root / "run.log"), str(root / "run.json"),
                 str(root / "artifact.json")],
                capture_output=True, text=True, check=True)
            self.assertTrue(json.loads(done.stdout)["strict_verified"])


if __name__ == "__main__":
    unittest.main()
