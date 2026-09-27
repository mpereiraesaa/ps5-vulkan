# SPDX-License-Identifier: GPL-3.0-or-later
"""Integer-dot fixture typing and real host compiler coverage, no GPU evidence.

All generated objects live in a temporary directory. The PSBC archive is read
only; this module does not write shared SDK/CTS build paths.
"""
import importlib.util
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("integer_dot_spirv", ROOT / "tools/integer_dot_spirv.py")
dot = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = dot
SPEC.loader.exec_module(dot)


def instructions(words):
    index = 5
    while index < len(words):
        size = words[index] >> 16
        if not size or index + size > len(words):
            raise ValueError("invalid instruction length")
        yield words[index] & 65535, words[index + 1:index + size]
        index += size


class IntegerDotFixtures(unittest.TestCase):
    def test_shapes_and_invalid_requests(self):
        cases = dot.cases()
        self.assertEqual(42, len(cases))
        self.assertEqual(42, len({case.name for case in cases}))
        self.assertEqual(18, sum(c.packed_types is None for c in cases))
        for args in (("s", 1), ("other", 4), ("s", 4, 2), ("float", 4, True),
                     ("s", 3, False, (False, False)), ("u", 4, False, (0, 1))):
            with self.subTest(args=args), self.assertRaises(ValueError):
                dot.Case(*args)

    def test_typed_runtime_operands_and_storage(self):
        for case in (*dot.cases(), dot.Case("float", 4)):
            with self.subTest(case=case.name):
                words = dot.module(case)
                self.assertEqual(words, dot.module(case))
                self.assertEqual(tuple(words), struct.unpack(f"<{len(words)}I", dot.binary(case)))
                self.assertEqual([0x07230203, 0x10300, 0], words[:3])
                ins = list(instructions(words))
                types = {args[0]: (op, args[1:]) for op, args in ins if 19 <= op <= 33}
                variables = {args[1]: args for op, args in ins if op == 59}
                loads = {args[1]: (args[0], args[2]) for op, args in ins if op == 61}
                accesses = {args[1]: args[2:] for op, args in ins if op == 65}
                bindings = {args[0]: args[2] for op, args in ins if op == 71 and args[1] == 33}
                strides = {args[0]: args[2] for op, args in ins if op == 71 and args[1] == 6}
                caps = {args[0] for op, args in ins if op == 17}
                self.assertEqual({1} if case.mode == "float" else
                                 {1, 6019, 6018 if case.packed_types is not None else 6016}, caps)
                ops = [(op, args) for op, args in ins if op == 148 or 4450 <= op <= 4455]
                self.assertEqual(1, len(ops))
                opcode, args = ops[0]
                self.assertEqual(148 if case.mode == "float" else
                                 {"s": 4450, "u": 4451, "su": 4452}[case.mode] + 3 * case.saturating, opcode)
                operands = args[2:]
                if case.packed_types is not None:
                    self.assertEqual(0, operands.pop())
                self.assertEqual(3 if case.saturating else 2, len(operands))
                self.assertEqual((22, [32]) if case.mode == "float" else
                                 (21, [32, int(case.mode != "u")]), types[args[0]])
                for binding, value in enumerate(operands):
                    element, access = loads[value]  # Must come from runtime storage.
                    variable = accesses[access][0]
                    self.assertEqual(binding, bindings[variable])
                    self.assertEqual(12, variables[variable][2])
                    if binding == 2:
                        self.assertEqual(args[0], element)
                    elif case.packed_types is not None:
                        self.assertEqual((21, [32, int(case.packed_types[binding])]), types[element])
                    else:
                        op, (scalar, width) = types[element]
                        self.assertEqual((23, case.components), (op, width))
                        expected = (22, [32]) if case.mode == "float" else (
                            21, [32, int(case.mode == "s" or (case.mode == "su" and binding == 0))])
                        self.assertEqual(expected, types[scalar])
                    pointer_type = variables[variable][0]
                    block = types[pointer_type][1][1]
                    array = types[block][1][0]
                    self.assertEqual(case.stride if binding < 2 else 4, strides[array])
                store, = [v for op, v in ins if op == 62]
                self.assertEqual(args[1], store[1])
                self.assertEqual(3, bindings[accesses[store[0]][0]])


class IntegerDotRealCompiler(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        archive = ROOT / "build/libpsbc.host.a"
        if not archive.is_file() or not (ROOT / "third_party/psbc-reference/libpsbc/psbc_compile.h").is_file():
            raise unittest.SkipTest("requires the pinned host PSBC build")
        directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(directory.cleanup)
        cls.tmp = Path(directory.name)
        cls.exe = cls.tmp / "integer-dot-compiler"
        includes = ["src", "include", "third_party/vulkan-headers/include", "third_party/psbc-reference", "third_party/opengnm/include"]
        sources = ["src/ps5vk_compiler.c", "src/ps5_compiler_shims.c", "tests/integer_dot_compiler.c"]
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        *["-I" + str(ROOT / p) for p in includes],
                        *[str(ROOT / p) for p in sources], str(archive), "-lstdc++", "-lm", "-lpthread",
                        "-o", str(cls.exe)], check=True, capture_output=True, text=True)

    def test_all_shapes_with_software_lowering(self):
        cases = (*dot.cases(), dot.Case("float", 4))
        paths = []
        for case in cases:
            path = self.tmp / (case.name + ".spv")
            path.write_bytes(dot.binary(case))
            paths.append(path)
        # The compiler's ACO program listing supplements compilation; it is
        # not a disassembly of emitted bytes or numerical hardware evidence.
        environment = {**os.environ, "PSBC_DEBUG_DISASM": "1"}
        result = subprocess.run([str(self.exe), *map(str, paths)], env=environment,
                                check=True, capture_output=True, text=True)
        records = re.findall(r"^DOT_COMPILED (.+) words=(\d+) bindings=(\d+)$", result.stdout, re.M)
        self.assertEqual(len(cases), len(records))
        for case, path, (name, code_words, bindings) in zip(cases, paths, records):
            self.assertEqual(str(path), name)
            self.assertGreater(int(code_words), 0)
            self.assertEqual(15 if case.saturating else 11, int(bindings))
        self.assertIn("After RA:", result.stderr)
        self.assertIn("v_mul_lo_u32", result.stderr)
        self.assertNotRegex(result.stderr, r"\bv_[su]*dot[248]")
