import hashlib
import unittest
from tools.verify_graphics_depth import REFERENCE_SELF, validate


class DepthAudit(unittest.TestCase):
    def setUp(self):
        self.messages = []
        for i in range(18):
            f, enabled = i % 6, int(i // 6 != 1)
            far = int(not enabled and f % 2 == 0)
            counts = [0, 0, 0]
            for n, value in enumerate((6, 0, 0) if far else (3, 2, 1)):
                counts[(f+n) % 3] = value
            self.messages += [
                f"PS5VK_DEPTH_INPUT frame={f} near_z=0.4 far_z=0.8 near_first={int(f % 2 == 0)} enabled={enabled}",
                f"PS5VK_GRAPHICS_SUBMIT serial={i+1} rc=0",
                f"PS5VK_GRAPHICS_COMPLETED serial={i+1}",
                "PS5VK_GRAPHICS_API_READBACK changed_words=6 valid=1 bad_alpha=0 bad_sum=0",
                f"PS5VK_TEXTURE_READBACK frame={f} red={counts[0]} green={counts[1]} blue={counts[2]} unexpected=0",
                f"PS5VK_TEXTURE_COMPONENT_ORDER frame={f} dominant={f % 3} valid=1",
                f"PS5VK_DEPTH_OCCLUSION frame={f} enabled={enabled} far_visible={far} valid=1",
                f"PS5VK_VIDEO_SUBMIT token={f+1} rc=0",
                f"PS5VK_VIDEO_PRESENTED token={f+1} matching_event=1",
                f"PS5VK_GRAPHICS_REUSE_END frame={f} slot={f % 2} displayed={f % 2}"]
        self.messages += ["PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0", "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE"]

    def audit(self):
        text = "HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=test\n"
        text += "".join(f"{i}\t{i}\tMARK\t{m}\n" for i, m in enumerate(self.messages, 1))
        text += f"BYE seq={len(self.messages)} reason=graphics-api-end\n"
        data = text.encode()
        meta = dict(sha256=hashlib.sha256(data).hexdigest(), clean=True, bye=True, gaps=[],
                    transport="tcp", protocol="ps5log/1", records=len(self.messages),
                    identity=dict(title="PPSA99994", app="ps5vk", boot="test"))
        return validate(data, meta, {"files": {"eboot.bin": REFERENCE_SELF}})

    def test_valid_synthetic_control(self):
        self.assertEqual(self.audit()["far_visible"], 3)

    def test_color_swap(self):
        self.messages[4] = "PS5VK_TEXTURE_READBACK frame=0 red=1 green=2 blue=3 unexpected=0"
        with self.assertRaisesRegex(ValueError, "occlusion colors"):
            self.audit()

    def test_disabled_depth_does_not_change_result(self):
        self.messages[64] = "PS5VK_TEXTURE_READBACK frame=0 red=3 green=2 blue=1 unexpected=0"
        with self.assertRaisesRegex(ValueError, "occlusion colors"):
            self.audit()

    def test_missing_completion(self):
        del self.messages[2]
        with self.assertRaisesRegex(ValueError, "event counts"):
            self.audit()

    def test_completion_before_submit(self):
        self.messages[1], self.messages[2] = self.messages[2], self.messages[1]
        with self.assertRaisesRegex(ValueError, "frame ordering"):
            self.audit()

    def test_leak(self):
        self.messages[-2] = "PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=131072"
        with self.assertRaisesRegex(ValueError, "cleanup"):
            self.audit()
