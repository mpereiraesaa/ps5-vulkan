import hashlib
import unittest
from tools.verify_vertex_bindings import validate, MASKS, COPY_BYTES


class VertexBindingsEvidence(unittest.TestCase):
    def messages(self):
        rows = ["PS5VK_GRAPHICS_LIMITS bindings=16"]
        for case, mask in enumerate(MASKS):
            control = [f"PS5VK_COMPUTE_RESULT round={r} checked=3072 outputs=0 guards=0" for r in range(6)]
            control += ["PS5VK_COMPUTE_END rounds=6 dispatches=12"]
            rows += control
            rows += [
                f"PS5VK_RUNTIME_GRAPHICS_CACHE rc=0 hit={case//3} compiled_pairs={min(case+1,3)} hits={case//3} misses={min(case+1,3)}",
                f"PS5VK_BINDINGS_INPUT case={case} mask={mask} declared=16 reversed_locations=1 distinct_buffers=1 odd_offsets=1",
                f"PS5VK_BINDINGS_PREPARED serial={case+1} mask={mask} copied_bytes={COPY_BYTES[case]}",
                f"PS5VK_GRAPHICS_SUBMIT serial={case+1} rc=0",
                f"PS5VK_GRAPHICS_COMPLETED serial={case+1}",
                f"PS5VK_BINDINGS_READBACK case={case} mask={mask} expected_white=471744 other=0 valid=1",
                "PS5VK_VIDEO_PRESENTED fence=0 matching_event=1",
                "PS5VK_GRAPHICS_REUSE_END displayed=0",
            ]
            rows += control
        return rows + ["PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
                       "PS5VK_GRAPHICS_API_CLEANUP_COMPLETE"]

    def fixture(self, messages):
        lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=abc"]
        lines += [f"{i}\t{i}\tMARK\t{msg}" for i, msg in enumerate(messages, 1)]
        lines += [f"BYE seq={len(messages)} reason=graphics-api-end"]
        log = ("\n".join(lines)+"\n").encode()
        receipt = {"sha256": hashlib.sha256(log).hexdigest(), "clean": True,
                   "bye": True, "gaps": [], "raw_lines": 0, "transport": "tcp",
                   "protocol": "ps5log/1", "records": len(messages), "last_seq": len(messages),
                   "identity": {"title": "PPSA99994", "app": "ps5vk", "boot": "abc"}}
        artifact = {"files": {"eboot.bin": "a"*64}, "scissor_probe": 13,
                    "stage": "graphics-api-native-presentation-reuse",
                    "compiler": "runtime-psbc-aco",
                    "graphics_shader_source": "owned-runtime-vertex-bindings",
                    "geometry_fixture": "sixteen-and-sparse-vertex-bindings",
                    "termination": "shell-close-after-cleanup"}
        return log, receipt, artifact

    def test_accepts_complete_witness_without_inventing_exit_or_deployment(self):
        result = validate(*self.fixture(self.messages()))
        self.assertEqual(result["cases"], 4)
        self.assertFalse(result["process_exit_verified"])
        self.assertFalse(result["deployment_identity_verified"])

    def test_rejects_semantic_corruption_even_with_valid_transport(self):
        mutations = (
            ("bindings=16", "bindings=1"),
            ("mask=8008", "mask=ffff"),
            ("copied_bytes=938", "copied_bytes=457"),
            ("expected_white=471744", "expected_white=471743"),
            ("other=0", "other=1"),
            ("outputs=0", "outputs=1"),
            ("guards=0", "guards=1"),
            ("matching_event=1", "matching_event=0"),
            ("displayed=0", "displayed=1"),
            ("hit=1", "hit=0"),
            ("serial=1", "serial=99"),
            ("allocations_bytes=0", "allocations_bytes=1"),
        )
        for before, after in mutations:
            with self.subTest(before=before):
                rows = self.messages()
                index = next(i for i, row in enumerate(rows) if before in row)
                rows[index] = rows[index].replace(before, after, 1)
                with self.assertRaises(ValueError):
                    validate(*self.fixture(rows))

    def test_rejects_missing_duplicate_and_reordered_cases(self):
        rows = self.messages()
        index = next(i for i, row in enumerate(rows) if "BINDINGS_READBACK" in row)
        for changed in (rows[:index]+rows[index+1:], rows[:index]+[rows[index]]+rows[index:]):
            with self.assertRaises(ValueError):
                validate(*self.fixture(changed))
        rows[index], rows[index-2] = rows[index-2], rows[index]
        with self.assertRaises(ValueError):
            validate(*self.fixture(rows))

    def test_rejects_incomplete_or_wrong_identity_stream(self):
        for key, value in (("sha256", "b"*64), ("clean", False), ("raw_lines", 1),
                           ("last_seq", 1), ("transport", "udp")):
            log, receipt, artifact = self.fixture(self.messages())
            receipt[key] = value
            with self.assertRaises(ValueError):
                validate(log, receipt, artifact)


if __name__ == "__main__":
    unittest.main()
