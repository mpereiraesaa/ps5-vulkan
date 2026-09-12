import hashlib
import unittest
from tools.verify_graphics_scene import REFERENCE_SELF, validate


class SceneEvidence(unittest.TestCase):
    def setUp(self):
        self.messages = []
        for f in range(180):
            self.messages += [
                f"PS5VK_COLOR_CLEAR_PREPARED serial={f+1} bgra=ff000000 bytes=8912896",
                f"PS5VK_GRAPHICS_SUBMIT serial={f+1} rc=0",
                f"PS5VK_GRAPHICS_COMPLETED serial={f+1}",
                "PS5VK_GRAPHICS_API_READBACK valid=1 bad_alpha=0 bad_sum=0 changed_words=3000 total_words=4000",
                f"PS5VK_TEXTURE_READBACK frame={f} red=1000 green=1000 blue=1000 unexpected=0",
                f"PS5VK_VIDEO_SUBMIT token={f+1} rc=0",
                f"PS5VK_VIDEO_PRESENTED token={f+1} fence=0 matching_event=1",
                f"PS5VK_GRAPHICS_REUSE_END frame={f} slot={f%2} displayed={f%2}"]
        self.messages.append("PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0")
        self.artifact = dict(files={"eboot.bin": REFERENCE_SELF}, scene="two-cubes",
                             exit_control=0, keep_agc_module=False)

    def audit(self):
        text = "HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=test\n"
        text += "".join(f"{i}\t{i}\tMARK\t{m}\n" for i, m in enumerate(self.messages, 1))
        text += f"BYE seq={len(self.messages)} reason=graphics-api-end\n"
        data = text.encode()
        meta = dict(sha256=hashlib.sha256(data).hexdigest(), clean=True, bye=True,
                    gaps=[], transport="tcp", protocol="ps5log/1", records=len(self.messages),
                    identity=dict(title="PPSA99994", app="ps5vk", boot="test"))
        return validate(data, meta, self.artifact)

    def test_valid_does_not_claim_exit_or_spatial_correctness(self):
        result = self.audit()
        self.assertEqual(result["frames"], 180)
        self.assertFalse(result["process_exit_verified"])
        self.assertFalse(result["spatial_correctness_verified"])

    def test_wrong_artifact(self):
        self.artifact["files"]["eboot.bin"] = "wrong"
        with self.assertRaisesRegex(ValueError, "artifact"): self.audit()

    def test_diagnostic(self):
        self.artifact["keep_agc_module"] = True
        with self.assertRaisesRegex(ValueError, "diagnostic"): self.audit()

    def test_missing_completion(self):
        del self.messages[2]
        with self.assertRaisesRegex(ValueError, "180 frames"): self.audit()

    def test_wrong_serial(self):
        self.messages[2] = "PS5VK_GRAPHICS_COMPLETED serial=2"
        with self.assertRaisesRegex(ValueError, "completion"): self.audit()

    def test_bad_order(self):
        self.messages[1], self.messages[2] = self.messages[2], self.messages[1]
        with self.assertRaisesRegex(ValueError, "frame order"): self.audit()

    def test_wrong_clear(self):
        self.messages[0] = self.messages[0].replace("ff000000", "55aa11ee")
        with self.assertRaisesRegex(ValueError, "clear"): self.audit()

    def test_unaccounted_pixels(self):
        self.messages[4] = self.messages[4].replace("red=1000", "red=999")
        with self.assertRaisesRegex(ValueError, "texture"): self.audit()

    def test_wrong_flip(self):
        self.messages[6] = self.messages[6].replace("token=1", "token=2")
        with self.assertRaisesRegex(ValueError, "present"): self.audit()

    def test_wrong_slot(self):
        self.messages[7] = self.messages[7].replace("displayed=0", "displayed=1")
        with self.assertRaisesRegex(ValueError, "reuse"): self.audit()

    def test_leak(self):
        self.messages[-1] = "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=65536"
        with self.assertRaisesRegex(ValueError, "cleanup"): self.audit()
