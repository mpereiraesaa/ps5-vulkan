"""Positive and negative controls for the artifact-bound D16 verifier."""
import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from tools.verify_d16_depth_witness import validate


def record(name, **fields):
    return name + " " + " ".join(f"{key}={value}" for key, value in fields.items())


class D16DepthWitnessTest(unittest.TestCase):
    def test_exact_artifact_and_two_depth_controls(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact = root / "eboot.bin"
            artifact.write_bytes(b"signed-d16-witness")
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "stage": "graphics-api-offscreen-draw", "runtime_sdk": True,
                "submit_enabled": True, "scissor_probe": 14,
                "d16_depth_witness": 1, "d16_depth_attachment_diagnostic": 1,
                "graphics": {"source": "experiments/graphics/scene3d.pipe"},
                "files": {"eboot.bin": digest},
            }))
            events = [record("PS5VK_GRAPHICS_API_DEVICE_CREATED"),
                      record("PS5VK_D16_IMAGE_QUERY", usage="depth-attachment",
                             max_extent="128x128", mip_levels=1, layers=1, samples=1)]
            for frame, (word, depth, changed) in enumerate(
                    (("ffffffff", "1.0", 2048), ("00000000", "0.0", 0))):
                events += [
                    record("PS5VK_D16_DEPTH_LOAD_CLEAR", frame=frame,
                           extent="128x128", usage="depth-attachment",
                           clear_word=word, depth=depth),
                    record("PS5VK_GRAPHICS_SUBMIT", serial=frame+1, rc=0),
                    record("PS5VK_GRAPHICS_COMPLETED", serial=frame+1,
                           image_bytes=65536),
                    record("PS5VK_DEPTH_CLEAR_WITNESS", frame=frame,
                           format=124, clear_depth_word=word,
                           expect_visible=1-frame, changed=changed,
                           total=32768, bad_alpha=0, valid=1),
                    record("PS5VK_GRAPHICS_API_READBACK", changed_words=changed,
                           total_words=32768, bad_alpha=0, bad_sum=0,
                           viewport="128x128", valid=1),
                ]
            events += [record("PS5VK_PLATFORM_CLOSE", rc=0, allocations_bytes=0),
                       record("PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")]
            log = root / "run.log"

            def write_log():
                lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=abc"]
                lines += [f"{index}\t{index}\tMARK\t{message}"
                          for index, message in enumerate(events, 1)]
                lines.append(f"BYE seq={len(events)} reason=graphics-api-end")
                raw = ("\n".join(lines) + "\n").encode()
                log.write_bytes(raw)
                log.with_suffix(".json").write_text(json.dumps({
                    "sha256": hashlib.sha256(raw).hexdigest(), "clean": True,
                    "bye": True, "gaps": [], "transport": "tcp",
                    "protocol": "ps5log/1", "records": len(events),
                    "identity": {"title": "PPSA99994", "app": "ps5vk",
                                 "boot": "abc"},
                }))

            write_log()
            self.assertEqual(validate(log, manifest, artifact)["occluded_pixels"], 0)
            events[-4] = events[-4].replace("valid=1", "valid=0")
            write_log()
            with self.assertRaisesRegex(ValueError, "depth draw oracle|GPU color readback"):
                validate(log, manifest, artifact)
            events[-4] = events[-4].replace("valid=0", "valid=1")
            write_log()
            artifact.write_bytes(b"different artifact")
            with self.assertRaisesRegex(ValueError, "signed eboot hash"):
                validate(log, manifest, artifact)


if __name__ == "__main__":
    unittest.main()
