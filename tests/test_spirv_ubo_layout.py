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


def group_annotations(words, selected=None):
    """Keep executable instructions intact; group selected layout annotations."""
    result = words[:5]
    groups = {}
    for at, size, op in instructions(words):
        decoration_at = 2 if op == 71 else 3
        if op not in (71, 72) or size <= decoration_at:
            result.extend(words[at:at + size])
            continue
        payload = tuple(words[at + decoration_at:at + size])
        if payload[0] not in (2, 3, 4, 5, 6, 7, 35) or (selected is not None and payload[0] not in selected):
            result.extend(words[at:at + size])
            continue
        key = op, payload
        if key not in groups:
            group = result[3]
            result[3] += 1
            groups[key] = group
            result.extend([(2 << 16) | 73, group])
            result.extend([((2 + len(payload)) << 16) | 71, group, *payload])
        group = groups[key]
        if op == 71:
            result.extend([(3 << 16) | 74, group, words[at + 1]])
        else:
            result.extend([(4 << 16) | 75, group, words[at + 1], words[at + 2]])
    # Merge applications to exercise multiple type targets or member pairs.
    applications = {}
    for at, size, op in instructions(result):
        if op in (74, 75):
            applications.setdefault((op, result[at + 1]), []).extend(result[at + 2:at + size])
    merged = result[:5]
    for at, size, op in instructions(result):
        if op in (74, 75):
            key = op, result[at + 1]
            targets = applications.pop(key, None)
            if targets is not None:
                merged.extend([((2 + len(targets)) << 16) | op, key[1], *targets])
        else:
            merged.extend(result[at:at + size])
    return merged


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

    def test_grouped_block_cannot_bypass_std140(self):
        compact = group_annotations(self.compile("std430"), {2})
        self.assertTrue(self.valid(compact, True))
        self.assertFalse(self.valid(compact, False))

    def test_grouped_type_and_member_layouts(self):
        for layout in ("std430", "std140"):
            for order in ("row_major", "column_major"):
                with self.subTest(layout=layout, order=order):
                    words = self.compile(layout)
                    if order == "column_major":
                        # Compile a proper column-major shader, not a decoration
                        # mutation with incompatible precomputed matrix strides.
                        source = self.directory / f"{layout}.comp"
                        source.write_text(source.read_text().replace("row_major", "column_major"))
                        binary = source.with_suffix(".spv")
                        subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.0",
                                        str(source), "-o", str(binary)], check=True,
                                       capture_output=True, text=True)
                        data = binary.read_bytes()
                        words = list(struct.unpack(f"<{len(data) // 4}I", data))
                    grouped = group_annotations(words)
                    self.assertTrue(self.valid(grouped, True))
                    self.assertEqual(self.valid(grouped, False), layout == "std140")
                    self.assertTrue(any(op == 75 and size > 4 for _, size, op in instructions(grouped)))

    def test_grouped_invalid_layout_payloads(self):
        original = group_annotations(self.compile("std430"))
        self.assertTrue(self.valid(original, True))
        for decoration in (6, 7, 35):
            at = next(at for at, size, op in instructions(original)
                      if op == 71 and size == 4 and original[at + 2] == decoration)
            with self.subTest(decoration=decoration):
                bad = original.copy()
                bad[at + 3] = 1
                self.assertFalse(self.valid(bad, True))

    def test_grouped_malformed_targets_and_members(self):
        original = group_annotations(self.compile("std430"))
        member = next(at for at, _, op in instructions(original) if op == 75)
        target = next(at for at, _, op in instructions(original) if op == 74)
        scalar = next(original[at + 1] for at, _, op in instructions(original) if op == 21)
        mutations = [(member + 3, 0xffffffff), (member + 2, scalar),
                     (member + 1, scalar), (target + 2, original[3]),
                     (target + 2, original[target + 1])]
        for index, value in mutations:
            bad = original.copy()
            bad[index] = value
            self.assertFalse(self.valid(bad, True))
        bad = original.copy()
        size = bad[member] >> 16
        # Remove the last member index, preserving subsequent instructions.
        del bad[member + size - 1]
        bad[member] = ((size - 1) << 16) | 75
        self.assertFalse(self.valid(bad, True))

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

    def test_array_stride_on_scalar_type_is_rejected(self):
        words = self.compile("std430")
        self.assertTrue(self.valid(words, True))
        scalar = next(words[at + 1] for at, size, op in instructions(words)
                      if op == 21 and size == 4 and words[at + 2] == 32)
        annotation = next(at for at, _, op in instructions(words) if op == 71)
        bad = words[:annotation] + [(4 << 16) | 71, scalar, 6, 4] + words[annotation:]
        self.assertFalse(self.valid(bad, True))

    def test_spec_constant_array_length_inside_ubo_is_rejected(self):
        words = self.compile("std430")
        self.assertTrue(self.valid(words, True))
        length_id = next(words[at + 3] for at, size, op in instructions(words)
                         if op == 28 and size == 4)
        constant = next(at for at, size, op in instructions(words)
                        if op == 43 and size == 4 and words[at + 2] == length_id)
        words[constant] = (4 << 16) | 50  # OpSpecConstant, same default literal.
        self.assertFalse(self.valid(words, True))


if __name__ == "__main__":
    unittest.main()
