import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from verify_imageless_framebuffer_witness import validate  # noqa: E402


class ImagelessFramebufferWitnessTests(unittest.TestCase):
    def setUp(self):
        self.artifact = {
            "title": "PPSA99994", "profile": "imageless-framebuffer-witness",
            "submit_enabled": True, "files": {"eboot.bin": "a" * 64},
            "imageless_framebuffer": {
                "feature": "VkPhysicalDeviceImagelessFramebufferFeatures.imagelessFramebuffer",
                "diagnostic_only": True, "views": 2, "framebuffers": 1,
                "submissions": 2, "extent": [64, 64],
                "format": "VK_FORMAT_R8G8B8A8_UNORM", "pixels_per_view": 4096,
            },
        }
        self.messages = [
            "PS5VK_CONSUMER_IMAGELESS_RESULT views=2 same_framebuffer=1 pixels=4096 "
            "first_mismatches=0 second_mismatches=0 valid=1",
            "PS5VK_CONSUMER_IMAGELESS_DRAW_RESULT views=2 draws=2 "
            "drawn_pixels=2048 clear_pixels=6144 mismatches=0 valid=1",
            "PS5VK_CONSUMER_TEST_SUCCESS",
            "PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1",
            "PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1",
        ]

    def run_validation(self):
        lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=123"]
        lines.extend(f"{index}\t{index}\tMARK\t{message}"
                     for index, message in enumerate(self.messages, 1))
        lines.append(f"BYE seq={len(self.messages)} reason=consumer-imageless-framebuffer-end")
        log = ("\n".join(lines) + "\n").encode()
        receipt = {"protocol": "ps5log/1", "transport": "tcp", "clean": True,
                   "bye": True, "gaps": [], "raw_lines": 0,
                   "sha256": hashlib.sha256(log).hexdigest(),
                   "last_seq": len(self.messages),
                   "identity": {"title": "PPSA99994", "app": "ps5vk", "boot": "123"}}
        return validate(log, receipt, self.artifact)

    def test_exact_run(self):
        self.assertEqual(self.run_validation()["pixels_checked"], 8192)
        self.assertEqual(self.run_validation()["drawn_pixels_checked"], 2048)

    def test_pixel_mismatch(self):
        self.messages[0] = self.messages[0].replace("second_mismatches=0", "second_mismatches=1")
        with self.assertRaisesRegex(ValueError, "IMAGELESS_RESULT"):
            self.run_validation()

    def test_missing_draw(self):
        self.messages.pop(1)
        with self.assertRaisesRegex(ValueError, "IMAGELESS_DRAW_RESULT"):
            self.run_validation()

    def test_wrong_draw_coverage(self):
        self.messages[1] = self.messages[1].replace("drawn_pixels=2048", "drawn_pixels=0")
        with self.assertRaisesRegex(ValueError, "IMAGELESS_DRAW_RESULT"):
            self.run_validation()

    def test_missing_retirement(self):
        self.messages.pop()
        with self.assertRaisesRegex(ValueError, "READY_FOR_SHELL_CLOSE"):
            self.run_validation()

    def test_public_profile_claim(self):
        self.artifact["imageless_framebuffer"]["diagnostic_only"] = False
        with self.assertRaisesRegex(ValueError, "artifact contract"):
            self.run_validation()

    def test_native_readback_uses_served_zero_offset_shape(self):
        witness = (ROOT / "examples/native_consumer/imageless_framebuffer_witness.h").read_text()
        self.assertIn("VkBufferImageCopy copy = {.bufferOffset = 0,", witness)
        self.assertIn(".size = BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT", witness)
        self.assertLess(witness.index("vkInvalidateMappedMemoryRanges(device"),
                        witness.index("PS5VK_CONSUMER_IMAGELESS_RESULT"))
        self.assertIn("vkCmdDraw(command, 3, 1, 0, 0)", witness)


if __name__ == "__main__":
    unittest.main()
