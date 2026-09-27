"""The SDK witness must exercise LocalSizeId, including specialization."""
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.maintenance4_local_size_spirv import instructions, local_size_id
from tools.build_maintenance4_local_size_witness import ordinary_environment


ROOT = Path(__file__).resolve().parents[1]


class LocalSizeIdFixture(unittest.TestCase):
    def test_builder_uses_ordinary_sdk(self):
        environment = ordinary_environment({"PS5VK_INTEGER_DOT_DIAGNOSTIC": "1",
            "PS5VK_INLINE_UNIFORM_DIAGNOSTIC": "1", "TASK_SENTINEL": "kept"}, "/tmp/sdk")
        self.assertNotIn("PS5VK_INTEGER_DOT_DIAGNOSTIC", environment)
        self.assertNotIn("PS5VK_INLINE_UNIFORM_DIAGNOSTIC", environment)
        self.assertEqual(environment["TASK_SENTINEL"], "kept")
        self.assertEqual(environment["PS5VK_USE_SDK"], "1")

    def test_specialized_execution_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "shader.spv"
            subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.0",
                str(ROOT / "experiments/compute/maintenance4_local_size.comp"),
                "-o", str(binary)], check=True, capture_output=True)
            source = binary.read_bytes()
        result = local_size_id(source)
        words = struct.unpack(f"<{len(result)//4}I", result)
        original = struct.unpack(f"<{len(source)//4}I", source)
        self.assertEqual(words[1], 0x00010200)
        self.assertEqual(words[3], original[3] + 2)
        x_id, one_id = original[3], original[3] + 1
        parsed = [(opcode, words[at+1:at+length])
                  for at, opcode, length in instructions(words)]
        self.assertIn((331, (4, 38, x_id, one_id, one_id)), parsed)
        self.assertIn((71, (x_id, 1, 0)), parsed)
        self.assertTrue(any(op == 50 and args[1:] == (x_id, 64) for op, args in parsed))
        self.assertTrue(any(op == 43 and args[1:] == (one_id, 1) for op, args in parsed))
        self.assertFalse(any(op == 16 and args[1] == 17 for op, args in parsed))

    def test_malformed_source_rejected(self):
        for data in (b"", b"garbage", b"\x00" * 20,
                     struct.pack("<6I", 0x07230203, 0x00010000, 0, 2, 0, 0)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                local_size_id(data)


if __name__ == "__main__":
    unittest.main()
