# SPDX-License-Identifier: GPL-3.0-or-later
"""Real integer-dot graphics compilation; no numerical GPU evidence.

Objects and shaders live in temporary directories; shared PSBC and headers
are read only, so this module requires no shared-output SERIAL slot.
"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from test_integer_dot_compiler import dot, instructions
from test_maintenance4_interfaces import shader as interface_shader, STAGES

ROOT = Path(__file__).resolve().parents[1]
GRAPHS = {"vert": ("vert", "frag"), "frag": ("vert", "frag"),
          "geom": ("vert", "geom", "frag"),
          "tesc": ("vert", "tesc", "tese", "frag"),
          "tese": ("vert", "tesc", "tese", "frag"), "all": STAGES}


def template(stage, components):
    source = interface_shader(stage, 3, 3)
    helper = f"""
layout(set=0,binding=0,std430) readonly buffer DotInput {{ vec4 dot_data[]; }};
float test_dot() {{ return dot(dot_data[0].{'xyzw'[:components]},
                              dot_data[1].{'xyzw'[:components]}) + dot_data[2].x; }}
"""
    source = source.replace("void main()", helper + "\nvoid main()")
    if stage == "geom":
        source = source.replace("EmitVertex();", "vout+=vec3(test_dot()); EmitVertex();")
    elif stage == "tesc":
        source = source.replace("if(gl_InvocationID==0)", "vout[gl_InvocationID]+=vec3(test_dot()); if(gl_InvocationID==0)")
    else:
        offset = source.rfind("}")
        addition = "color+=vec4(vec3(test_dot()),0);" if stage == "frag" else "vout+=vec3(test_dot());"
        source = source[:offset] + addition + source[offset:]
    return source


class IntegerDotGraphics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        archive = ROOT / "build/libpsbc.host.a"
        cls.glslang = shutil.which("glslangValidator")
        fallback = ROOT / "build/runtime-graphics/toolchain/usr/bin/glslangValidator"
        if not cls.glslang and fallback.is_file():
            cls.glslang = str(fallback)
        gears = Path(os.environ.get("LAB_SIBLINGS", ROOT.parent)) / "ps5-agc-gears"
        if not archive.is_file() or not cls.glslang or not (gears / "include").is_dir():
            raise unittest.SkipTest("requires built host PSBC, glslang and graphics headers")
        directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(directory.cleanup)
        cls.tmp = Path(directory.name)
        cls.exe = cls.tmp / "integer-dot-graphics"
        includes = [ROOT / "src", ROOT / "native", gears / "src", gears / "include",
                    ROOT / "third_party/vulkan-headers/include", ROOT / "third_party/psbc-reference"]
        sources = ["tests/integer_dot_graphics.c", "native/runtime_shader.c", "native/runtime_graphics_compiler.c",
                   "src/color_attachment_contract.c", "src/spirv_graphics_interface.c", "src/texture_format.c",
                   "src/ps5_compiler_shims.c"]
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        *["-I" + str(p) for p in includes], *[str(ROOT / p) for p in sources], str(archive),
                        "-lstdc++", "-lm", "-lpthread", "-o", str(cls.exe)], check=True, capture_output=True, text=True)
        cls.controls, cls.templates = {}, {}
        for stage in STAGES:
            for n in (0, 2, 3, 4):
                path = cls.tmp / f"base{n}.{stage}"
                path.write_text(interface_shader(stage, 3, 3) if not n else template(stage, n))
                out = Path(str(path) + ".spv")
                subprocess.run([cls.glslang, "-V", str(path), "-o", str(out)], check=True, capture_output=True, text=True)
                if not n:
                    cls.controls[stage] = out
                else:
                    data = out.read_bytes()
                    cls.templates[stage, n] = (out, list(struct.unpack(f"<{len(data) // 4}I", data)))
        cls.variants = {}
        for case in dot.cases():
            for stage in STAGES:
                words = dot.integerize_dot_add(cls.templates[stage, case.components][1], case)
                path = cls.tmp / f"{case.name}.{stage}.spv"
                path.write_bytes(struct.pack(f"<{len(words)}I", *words))
                cls.variants[stage, case] = path

    def inspect(self, target, variants):
        graph = GRAPHS[target]
        modules = {stage: variants.get(stage, self.controls[stage]) for stage in graph}
        result = subprocess.run([str(self.exe), *[str(modules.get(stage, "-")) for stage in STAGES]],
                                check=True, capture_output=True, text=True)
        observed = tuple(map(int, result.stdout.split()))
        active = set(variants)
        if "tesc" in graph:
            expected = (0, int(bool(active & {"vert", "tesc"})),
                        int(bool(active & {"tese", "geom"})), int("frag" in active))
        else:
            expected = (int(bool(active & {"vert", "geom"})), 0, 0, int("frag" in active))
        self.assertEqual(expected, observed, result.stderr)

    def test_float_controls_use_the_same_resource_paths(self):
        for target in GRAPHS:
            with self.subTest(target=target):
                stages = STAGES if target == "all" else (target,)
                self.inspect(target, {stage: self.templates[stage, 4][0] for stage in stages})

    def test_integer_dot_each_stage_and_merged_graph(self):
        for case in dot.cases():
            for target in GRAPHS:
                with self.subTest(case=case.name, target=target):
                    stages = STAGES if target == "all" else (target,)
                    self.inspect(target, {stage: self.variants[stage, case] for stage in stages})

    def test_template_transform_rejects_missing_and_ambiguous_dot(self):
        words = self.templates["frag", 4][1]
        case = dot.Case("s", 4)
        with self.assertRaisesRegex(ValueError, "vector shape"):
            dot.integerize_dot_add(words, dot.Case("s", 2))
        malformed = list(words); malformed[5] = 0
        with self.assertRaisesRegex(ValueError, "malformed"):
            dot.integerize_dot_add(malformed, case)
        with self.assertRaisesRegex(ValueError, "exactly one"):
            dot.integerize_dot_add(dot.module(case), case)
        duplicate = list(words)
        for opcode, args in instructions(words):
            if opcode == 148:
                duplicate += dot.instruction(opcode, *args)
                break
        with self.assertRaisesRegex(ValueError, "exactly one"):
            dot.integerize_dot_add(duplicate, case)
