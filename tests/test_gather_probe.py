import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from tools.verify_gather_probe import expected_pixel, validate


class GatherFixture:
    def __init__(self, form=1):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.artifact = self.root / "eboot.bin"
        self.artifact.write_bytes(b"artifact")
        self.manifest = self.root / "manifest.json"
        self.run = self.root / "run.log"
        self.form = form
        self.source_by_form = {
            1: "experiments/graphics/runtime_gather_core.frag",
            2: "experiments/graphics/runtime_gather_const_offset.frag",
            3: "experiments/graphics/runtime_gather_dynamic_offset.frag",
            4: "experiments/graphics/runtime_gather_four_offsets.frag",
            5: "experiments/graphics/runtime_gather_component_0.frag",
            6: "experiments/graphics/runtime_gather_component_1.frag",
            7: "experiments/graphics/runtime_gather_component_2.frag",
            8: "experiments/graphics/runtime_gather_component_3.frag",
        }
        self.rewrite()

    def rewrite(self, actual=None):
        expected = expected_pixel(self.form)
        if actual is None:
            actual = expected
        witness = {
            "form": self.form,
            "texture_extent": [64, 64],
            "sample_coordinate": [0.5, 0.5],
            "texel_pattern": "rgba8-x-y-3x-plus-5y",
            "diagnostic_feature": self.form in (2, 3, 4),
            "profile_query_logged": True,
            "offset_limits": ({"min": -8, "max": 7}
                              if self.form in (2, 3, 4)
                              else {"min": 0, "max": 0}),
            "gpu_readback": True,
            "source": self.source_by_form[self.form],
        }
        source_hash = hashlib.sha256((Path(__file__).resolve().parents[1] /
                                      self.source_by_form[self.form]).read_bytes()).hexdigest()
        self.manifest.write_text(json.dumps({
            "stage": "graphics-api-offscreen-draw",
            "runtime_sdk": True,
            "submit_enabled": True,
            "scissor_probe": 15,
            "gather_probe": witness,
            "graphics_shader_source": f"owned-runtime-image-gather-{self.form}",
            "runtime_graphics_inputs": {"fragment": {"glsl_sha256": source_hash}},
            "files": {"eboot.bin": hashlib.sha256(self.artifact.read_bytes()).hexdigest()},
        }))
        messages = [
            f"PS5VK_GATHER_PROFILE extended={int(self.form in (2, 3, 4))} min_offset={-8 if self.form in (2, 3, 4) else 0} max_offset={7 if self.form in (2, 3, 4) else 0}",
            "PS5VK_GRAPHICS_API_DEVICE_CREATED",
            "PS5VK_GATHER_INPUT extent=64x64 channels=R:x,G:y,B:3x+5y,A:255",
            "PS5VK_GRAPHICS_COMPLETED serial=1 image_bytes=0",
            f"PS5VK_GATHER_READBACK form={self.form} expected_bgra={expected:08x} actual_bgra={actual:08x} changed_words=1 first_word_index=104 valid={int(actual == expected)}",
            "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
        ]
        identity = {"title": "PPSA99994", "app": "ps5vk", "boot": "boot0"}
        records = [f"{i}\t{i}\tMARK\t{message}" for i, message in enumerate(messages, 1)]
        lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=boot0", *records,
                 f"BYE seq={len(messages)} reason=graphics-api-end"]
        data = ("\n".join(lines) + "\n").encode()
        self.run.write_bytes(data)
        receipt = {
            "sha256": hashlib.sha256(data).hexdigest(),
            "clean": True,
            "bye": True,
            "gaps": [],
            "transport": "tcp",
            "protocol": "ps5log/1",
            "identity": identity,
            "records": len(messages),
        }
        self.run.with_suffix(".json").write_text(json.dumps(receipt))

    def close(self):
        self.tmp.cleanup()


class TestGatherProbe(unittest.TestCase):
    def test_expected_grid_readback_for_each_form(self):
        self.assertEqual(expected_pixel(1), 0x1F1F2020)
        self.assertEqual(expected_pixel(2), 0x03080B06)
        self.assertEqual(expected_pixel(3), 0xF5FAFDF8)
        self.assertEqual(expected_pixel(4), 0x30B8E503)
        self.assertEqual(expected_pixel(5), 0x1F1F2020)
        self.assertEqual(expected_pixel(6), 0x1F20201F)
        self.assertEqual(expected_pixel(7), 0xF8FD00FB)
        self.assertEqual(expected_pixel(8), 0xFFFFFFFF)

    def test_accepts_artifact_bound_gpu_readback(self):
        for form in range(1, 9):
            fixture = GatherFixture(form)
            try:
                with self.subTest(form=form):
                    result = validate(fixture.run, fixture.manifest, fixture.artifact)
                    self.assertEqual(result["form"], form)
                    self.assertEqual(int(result["expected_bgra"], 16), expected_pixel(form))
            finally:
                fixture.close()

    def test_rejects_wrong_gpu_pixel_and_edited_artifact(self):
        fixture = GatherFixture(2)
        try:
            fixture.rewrite(actual=expected_pixel(2) ^ 1)
            with self.assertRaises(ValueError):
                validate(fixture.run, fixture.manifest, fixture.artifact)
            fixture.rewrite()
            fixture.artifact.write_bytes(b"changed artifact")
            with self.assertRaises(ValueError):
                validate(fixture.run, fixture.manifest, fixture.artifact)
        finally:
            fixture.close()

    def test_rejects_manifest_bound_to_another_shader(self):
        fixture = GatherFixture(3)
        try:
            data = json.loads(fixture.manifest.read_text())
            data["gather_probe"]["source"] = fixture.source_by_form[2]
            fixture.manifest.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                validate(fixture.run, fixture.manifest, fixture.artifact)

            fixture.rewrite()
            data = json.loads(fixture.manifest.read_text())
            data["runtime_graphics_inputs"]["fragment"]["glsl_sha256"] = "0" * 64
            fixture.manifest.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                validate(fixture.run, fixture.manifest, fixture.artifact)
        finally:
            fixture.close()


if __name__ == "__main__":
    unittest.main()
