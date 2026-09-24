import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from tools.verify_sampler_mirror import CASES, expected_pixel, validate


class MirrorSamplerVerifier(unittest.TestCase):
    def fixture(self, root, case=11, actual=None):
        root = Path(root)
        artifact = root / "eboot.bin"
        artifact.write_bytes(b"signed witness identity")
        axis, filtering, u, v = CASES[case - 8]
        expected = expected_pixel(axis, filtering, u, v)
        if actual is None:
            actual = expected
        axis_index = {"u": 1, "v": 2, "w": 3}[axis]
        name = f"mirror-{axis}-{filtering}-" + (
            "negative" if (u if axis in ("u", "w") else v) < 0 else
            "positive" if (u if axis in ("u", "w") else v) > 1 else "inside")
        messages = [
            "PS5VK_GRAPHICS_API_DEVICE_CREATED",
            f"PS5VK_SAMPLER_CORE_INPUT case={case} name={name} uv_milli={int(u*1000)} "
            f"uv_v_milli={int(v*1000)} mirror_axis={axis_index} "
            f"filter={filtering} minification=0 expected_bgra={expected:08x}",
        ]
        if axis == "w":
            messages.append("PS5VK_LAYERED_INPUT target=3d slices=2 width=2 height=2")
        messages += [
            "PS5VK_TEXTURE_UPLOAD frame=0 pattern=" +
            ("layered-rgb" if axis == "w" else "rgb-cycle") +
            " width=2 height=2 slices=" + ("2" if axis == "w" else "1") +
            " levels=1 format=37",
            "PS5VK_GRAPHICS_SUBMIT serial=1 rc=0",
            "PS5VK_GRAPHICS_COMPLETED serial=1",
            f"PS5VK_SAMPLER_CORE_READBACK case={case} name={name} "
            f"expected_bgra={expected:08x} actual_bgra={actual:08x} "
            "expected=373248 other=0 valid=1",
            "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
            "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE",
        ]
        lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=fixture"]
        lines += [f"{i}\t{i}\tMARK\t{message}"
                  for i, message in enumerate(messages, 1)]
        lines.append(f"BYE seq={len(messages)} reason=graphics-api-end")
        log = ("\n".join(lines) + "\n").encode()
        run = root / "run.json"
        run.with_suffix(".log").write_bytes(log)
        run.write_text(json.dumps({"sha256": hashlib.sha256(log).hexdigest(),
                                   "clean": True, "bye": True, "gaps": [],
                                   "transport": "tcp", "protocol": "ps5log/1",
                                   "records": len(messages),
                                   "identity": {"title": "PPSA99994", "app": "ps5vk",
                                                "boot": "fixture"}}))
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps({
            "stage": "graphics-api-offscreen-draw", "runtime_sdk": True,
            "submit_enabled": True, "scissor_probe": 6,
            "sampler_mirror_case": case,
            "image_target": "3d" if axis == "w" else None,
            "geometry_fixture": "sampler-mirror-w-3d" if axis == "w" else "sampler-core-addressing",
            "t09_diagnostics": {"PS5VK_SAMPLER_MIRROR_CLAMP_DIAGNOSTIC": True},
            "termination": "shell-close-after-cleanup",
            "graphics": {"source": "experiments/graphics/scene3d-mirror-w.pipe" if axis == "w"
                         else "experiments/graphics/scene3d.pipe"},
            "files": {"eboot.bin": hashlib.sha256(artifact.read_bytes()).hexdigest()},
        }))
        return run, manifest, artifact

    def test_reference_covers_all_axes_filters_and_regions(self):
        expected = [0xffff0000, 0xffff0000, 0xff00ff00,
                    0xffbf4000, 0xffbf4000, 0xff00ff00,
                    0xffff0000, 0xffff0000, 0xff0000ff,
                    0xffbf0040, 0xffbf0040, 0xff0000ff,
                    0xffff0000, 0xffff0000, 0xff00ff00,
                    0xffbf4000, 0xffbf4000, 0xff00ff00]
        self.assertEqual([expected_pixel(*case) for case in CASES], expected)

    def test_accepts_one_artifact_bound_sdk_readback(self):
        with TemporaryDirectory() as directory:
            run, manifest, artifact = self.fixture(directory)
            result = validate(run, manifest, artifact)
            self.assertTrue(result["strict_verified"])
            self.assertEqual(result["case"], 11)

    def test_accepts_w_3d_nearest_and_linear(self):
        with TemporaryDirectory() as directory:
            for case in range(20, 26):
                run, manifest, artifact = self.fixture(directory, case=case)
                self.assertEqual(validate(run, manifest, artifact)["case"], case)

    def test_rejects_wrong_gpu_color_and_wrong_artifact(self):
        with TemporaryDirectory() as directory:
            run, manifest, artifact = self.fixture(directory, actual=0xff00ff00)
            with self.assertRaisesRegex(ValueError, "GPU sampler readback"):
                validate(run, manifest, artifact)
            artifact.write_bytes(b"different signed witness")
            with self.assertRaisesRegex(ValueError, "signed eboot hash"):
                validate(run, manifest, artifact)


if __name__ == "__main__":
    unittest.main()
