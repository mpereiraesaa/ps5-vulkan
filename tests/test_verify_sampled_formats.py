import hashlib
import unittest

from tools.verify_sampled_formats import CASES, validate


class SampledFormatVerifier(unittest.TestCase):
    def fixture(self):
        messages = []
        for case, (name, format_number, texel_bytes, expected) in enumerate(CASES):
            messages += [f"PS5VK_COMPUTE_RESULT round={round_index} checked=3072 outputs=0 guards=0"
                         for round_index in range(6)]
            messages += ["PS5VK_COMPUTE_END rounds=6 dispatches=12"]
            messages += [
                f"PS5VK_SAMPLED_FORMAT_INPUT case={case} name={name} format={format_number} bytes_per_texel={texel_bytes} expected_bgra={expected}",
                f"PS5VK_GRAPHICS_SUBMIT serial={case+1} rc=0",
                f"PS5VK_GRAPHICS_COMPLETED serial={case+1} image_bytes=8912896",
                f"PS5VK_SAMPLED_FORMAT_READBACK case={case} name={name} format={format_number} expected_bgra={expected} expected=373248 other=0 first_other=00000000 valid=1",
                f"PS5VK_VIDEO_PRESENTED token={case+1} fence=0 matching_event=1",
                f"PS5VK_GRAPHICS_REUSE_END frame=0 slot=0 displayed=0",
            ]
            messages += [f"PS5VK_COMPUTE_RESULT round={round_index} checked=3072 outputs=0 guards=0"
                         for round_index in range(6)]
            messages += ["PS5VK_COMPUTE_END rounds=6 dispatches=12"]
        messages += ["PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
                     "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE"]
        rows = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=abc"]
        rows += [f"{i}\t{i}\tMARK\t{message}" for i, message in enumerate(messages, 1)]
        rows.append(f"BYE seq={len(messages)} reason=graphics-api-end")
        log = ("\n".join(rows) + "\n").encode()
        metadata = {"sha256": hashlib.sha256(log).hexdigest(), "clean": True,
                    "bye": True, "gaps": [], "transport": "tcp",
                    "protocol": "ps5log/1", "records": len(messages),
                    "identity": {"title": "PPSA99994", "app": "ps5vk", "boot": "abc"}}
        artifact = {"stage": "graphics-api-native-presentation-reuse", "scissor_probe": 7,
                    "geometry_fixture": "sampled-format-candidates",
                    "termination": "shell-close-after-cleanup",
                    "files": {"eboot.bin": "a" * 64}}
        return log, metadata, artifact

    def test_accepts_complete_gpu_witness(self):
        result = validate(*self.fixture())
        self.assertEqual(result["cases"], 23)
        self.assertEqual(result["formats"], [case[0] for case in CASES])

    def test_rejects_wrong_srgb_decode(self):
        log, metadata, artifact = self.fixture()
        log = log.replace(b"expected_bgra=ff370d04 expected=373248",
                          b"expected_bgra=ff204080 expected=373248", 1)
        metadata["sha256"] = hashlib.sha256(log).hexdigest()
        with self.assertRaisesRegex(ValueError, "case identity"):
            validate(log, metadata, artifact)

    def test_rejects_incomplete_case(self):
        log, metadata, artifact = self.fixture()
        line = next(line for line in log.splitlines(keepends=True)
                    if b"PS5VK_SAMPLED_FORMAT_READBACK case=1" in line)
        log = log.replace(line, b"", 1)
        metadata["sha256"] = hashlib.sha256(log).hexdigest()
        with self.assertRaises(ValueError):
            validate(log, metadata, artifact)


if __name__ == "__main__":
    unittest.main()
