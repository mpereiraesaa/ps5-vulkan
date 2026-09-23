"""Vulkan base/extended UBO alignment and malformed SPIR-V decorations."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SHADER = """#version 450
#extension GL_EXT_scalar_block_layout : require
layout(local_size_x=1) in;
struct Inner { vec2 pair[2]; uint tail; };
layout({layout},set=0,binding=0) uniform Input {
    uint small[3];
    vec2 pairs[2];
    layout(row_major) mat2x3 matrix;
    Inner nested[2];
} u;
layout(set=0,binding=1) buffer Output { uint result[]; } outputData;
void main() {
    outputData.result[0] = u.small[1] +
        floatBitsToUint(u.pairs[1].x) +
        floatBitsToUint(u.matrix[0].x) + u.nested[1].tail;
}
"""


def instructions(words):
    at = 5
    while at < len(words):
        size, opcode = words[at] >> 16, words[at] & 0xffff
        assert size and at + size <= len(words)
        yield at, size, opcode
        at += size
    assert at == len(words)


class UboLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("glslangValidator") or not shutil.which("cc"):
            raise unittest.SkipTest("glslangValidator and C compiler required")
        cls.tempdir = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.tempdir.name)
        library = cls.directory / "layout.so"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-fPIC", "-shared", "-Isrc",
                        "src/spirv_ubo_layout.c", "-o", str(library)],
                       cwd=ROOT, check=True, capture_output=True, text=True)
        cls.validate = ctypes.CDLL(str(library)).ps5vk_spirv_validate_ubo_layout
        cls.validate.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t,
                                 ctypes.c_int]
        cls.validate.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.tempdir.cleanup()

    def compile(self, layout):
        source = self.directory / f"{layout}.comp"
        binary = self.directory / f"{layout}.spv"
        source.write_text(SHADER.replace("{layout}", layout))
        subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.0",
                        str(source), "-o", str(binary)], check=True,
                       capture_output=True, text=True)
        data = binary.read_bytes()
        return list(struct.unpack(f"<{len(data) // 4}I", data))

    def valid(self, words, standard):
        array = (ctypes.c_uint32 * len(words))(*words)
        return bool(self.validate(array, len(words), int(standard)))

    def test_compact_and_legacy_with_nested_struct_and_row_major_matrix(self):
        compact = self.compile("std430")
        legacy = self.compile("std140")
        self.assertTrue(self.valid(compact, True))
        self.assertFalse(self.valid(compact, False))
        self.assertTrue(self.valid(legacy, False))
        self.assertTrue(self.valid(legacy, True))

    def test_reject_invalid_array_stride_matrix_stride_and_offset(self):
        original = self.compile("std430")
        self.assertTrue(self.valid(original, True))
        mutations = []
        for at, size, op in instructions(original):
            if op == 71 and size == 4 and original[at + 2] == 6 and original[at + 3] == 8:
                bad = original.copy()
                bad[at + 3] = 1
                mutations.append(("array", bad))
                break
        for at, size, op in instructions(original):
            if op == 72 and size == 5 and original[at + 3] == 7:
                bad = original.copy()
                bad[at + 4] = 1
                mutations.append(("matrix", bad))
                break
        for at, size, op in instructions(original):
            if op == 72 and size == 5 and original[at + 3] == 35 and original[at + 4]:
                bad = original.copy()
                bad[at + 4] = 1
                mutations.append(("offset", bad))
                break
        self.assertEqual({name for name, _ in mutations}, {"array", "matrix", "offset"})
        for name, bad in mutations:
            with self.subTest(name=name):
                self.assertFalse(self.valid(bad, True))

    def test_truncated_instruction_and_excessive_id_bound(self):
        words = self.compile("std430")
        for at, size, op in instructions(words):
            if op == 71 and size == 4 and words[at + 2] == 6:
                words[at] = ((len(words) + 1) << 16) | op
                break
        self.assertFalse(self.valid(words, True))
        words[3] = 262145
        self.assertFalse(self.valid(words, True))


if __name__ == "__main__":
    unittest.main()
