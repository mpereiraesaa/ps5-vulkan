import hashlib
import unittest

from tools.verify_vertex_formats import CASES, validate


class VertexFormatVerifier(unittest.TestCase):
    def fixture(self):
        messages = []
        for case, (name, format_number, numeric, components, byte_count, word) in enumerate(CASES):
            messages += [f"PS5VK_COMPUTE_RESULT round={round_index} outputs=0 guards=0"
                         for round_index in range(6)]
            messages += ["PS5VK_COMPUTE_END rounds=6 dispatches=12"]
            messages += [
                f"PS5VK_VERTEX_FORMAT_INPUT case={case} name={name} format={format_number} numeric={numeric} components={components} bytes={byte_count} stride={byte_count} binding_offset=25 word={word}",
                f"PS5VK_VERTEX_BOUNCE serial={case+1} draw=0 bytes=359 alignment=4",
                f"PS5VK_GRAPHICS_SUBMIT serial={case+1} rc=0",
                f"PS5VK_GRAPHICS_COMPLETED serial={case+1} image_bytes=8912896",
                f"PS5VK_VERTEX_FORMAT_READBACK case={case} name={name} format={format_number} numeric={numeric} components={components} expected_white=471744 other=0 first_other=00000000 valid=1",
                f"PS5VK_VIDEO_PRESENTED token={case+1} fence=0 matching_event=1",
                "PS5VK_GRAPHICS_REUSE_END frame=0 slot=0 displayed=0",
            ]
            messages += [f"PS5VK_COMPUTE_RESULT round={round_index} outputs=0 guards=0"
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
        artifact = {"stage": "graphics-api-native-presentation-reuse", "scissor_probe": 8,
                    "geometry_fixture": "vertex-format-cases", "compiler": "runtime-psbc-aco",
                    "graphics_shader_source": "owned-runtime-vertex-formats",
                    "runtime_graphics_inputs": {"vertex_sint": {}, "vertex_uint": {},
                                                "vertex_unorm": {}, "fragment": {}},
                    "termination": "shell-close-after-cleanup", "files": {"eboot.bin": "a" * 64}}
        return log, metadata, artifact

    def test_accepts_complete_gpu_witness(self):
        result = validate(*self.fixture())
        self.assertEqual(result["cases"], 41)
        self.assertTrue(result["component_completion"])
        self.assertTrue(result["normalized_channel_order"])
        self.assertTrue(result["byte_granular_vertex_input"])

    def test_rejects_wrong_component_completion(self):
        log, metadata, artifact = self.fixture()
        log = log.replace(b"expected_white=471744 other=0 first_other=00000000 valid=1",
                          b"expected_white=353808 other=117936 first_other=ff00ffff valid=0", 1)
        metadata["sha256"] = hashlib.sha256(log).hexdigest()
        with self.assertRaisesRegex(ValueError, "GPU vertex-format oracle"):
            validate(log, metadata, artifact)

    def test_rejects_missing_case(self):
        log, metadata, artifact = self.fixture()
        line = next(line for line in log.splitlines(keepends=True)
                    if b"PS5VK_VERTEX_FORMAT_READBACK case=5" in line)
        log = log.replace(line, b"", 1)
        metadata["sha256"] = hashlib.sha256(log).hexdigest()
        with self.assertRaises(ValueError):
            validate(log, metadata, artifact)

    def test_rejects_wrong_packed_channel_witness(self):
        log, metadata, artifact = self.fixture()
        log = log.replace(b"case=9 name=bgra8-unorm format=44 numeric=float components=4 bytes=4 stride=4 binding_offset=25 word=ffaa5511",
                          b"case=9 name=bgra8-unorm format=44 numeric=float components=4 bytes=4 stride=4 binding_offset=25 word=ff1155aa", 1)
        metadata["sha256"] = hashlib.sha256(log).hexdigest()
        with self.assertRaisesRegex(ValueError, "case identity"):
            validate(log, metadata, artifact)

    def test_rejects_reintroduced_dword_binding_alignment(self):
        log, metadata, artifact = self.fixture()
        log = log.replace(b"binding_offset=25", b"binding_offset=24", 1)
        metadata["sha256"] = hashlib.sha256(log).hexdigest()
        with self.assertRaisesRegex(ValueError, "case identity"):
            validate(log, metadata, artifact)


if __name__ == "__main__":
    unittest.main()
