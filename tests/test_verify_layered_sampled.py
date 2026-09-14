import hashlib
import unittest

from tools.verify_layered_sampled import TARGETS, validate


class LayeredSampledVerifier(unittest.TestCase):
    def fixture(self, target="2d-array"):
        expected = TARGETS[target]
        messages = [
            f"PS5VK_LAYERED_QUERY target={target} dimension={expected['dimension']} depth={expected['depth']} layers={expected['layers']}",
            "PS5VK_COMPUTE_END rounds=6 dispatches=12",
            f"PS5VK_LAYERED_INPUT target={target} slices={expected['slices']} width=64 height=64",
            "PS5VK_GRAPHICS_SUBMIT serial=1 rc=0",
            "PS5VK_GRAPHICS_COMPLETED serial=1 image_bytes=8912896",
            f"PS5VK_LAYERED_READBACK target={target} red=1 green=2 blue=3 unexpected=0 valid=1",
            "PS5VK_VIDEO_PRESENTED token=1 fence=0 matching_event=1",
            "PS5VK_GRAPHICS_REUSE_END frame=0 slot=0 displayed=0",
            "PS5VK_COMPUTE_END rounds=6 dispatches=12",
            "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
            "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE",
        ]
        rows = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=abc"]
        rows += [f"{i}\t{i}\tMARK\t{message}" for i, message in enumerate(messages, 1)]
        rows.append(f"BYE seq={len(messages)} reason=graphics-api-end")
        log = ("\n".join(rows) + "\n").encode()
        metadata = {"sha256": hashlib.sha256(log).hexdigest(), "clean": True,
                    "bye": True, "gaps": [], "transport": "tcp",
                    "protocol": "ps5log/1", "records": len(messages),
                    "identity": {"title": "PPSA99994", "app": "ps5vk", "boot": "abc"}}
        artifact = {"stage": "graphics-api-native-presentation-reuse", "scissor_probe": 11,
                    "image_target": target, "geometry_fixture": expected["fixture"],
                    "termination": "return-main", "files": {"eboot.bin": "a" * 64}}
        return log, metadata, artifact

    def test_accepts_each_target(self):
        for target in TARGETS:
            with self.subTest(target=target):
                self.assertTrue(validate(*self.fixture(target), target)["gpu_readback"])

    def test_rejects_missing_color(self):
        log, metadata, artifact = self.fixture()
        log = log.replace(b"red=1", b"red=0")
        metadata["sha256"] = hashlib.sha256(log).hexdigest()
        with self.assertRaisesRegex(ValueError, "GPU layered readback"):
            validate(log, metadata, artifact, "2d-array")

    def test_rejects_wrong_artifact_target(self):
        log, metadata, artifact = self.fixture()
        artifact["image_target"] = "cube"
        with self.assertRaisesRegex(ValueError, "artifact profile"):
            validate(log, metadata, artifact, "2d-array")


if __name__ == "__main__":
    unittest.main()
