"""Real host compilation of widened interfaces; never native GPU evidence.

Every artifact is built in a temporary directory. The host PSBC archive and
sibling headers are read only, so this test needs no shared-output SERIAL slot.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STAGES = ("vert", "tesc", "tese", "geom", "frag")
GRAPHS = (("vert", "frag"), ("vert", "geom", "frag"),
          ("vert", "tesc", "tese", "frag"), STAGES)


def shader(stage, ins, outs, numeric="float", patch=None):
    prefix = {"float": "vec", "int": "ivec", "uint": "uvec"}[numeric]
    input_type, output_type = f"{prefix}{ins}", f"{prefix}{outs}"
    flat = "" if numeric == "float" else "flat "

    def total(index=""):
        return "(" + "+".join("vin" + index + "." + c for c in "xyzw"[:ins]) + ")"

    header = "#version 450\n"
    if stage == "vert":
        value = "float(gl_VertexIndex+1)/8.0" if numeric == "float" else f"{numeric}((gl_VertexIndex+1)*64)"
        return header + f"""
layout(location=0) {flat}out {output_type} vout;
void main() {{
    const vec2 p[3]=vec2[3](vec2(-.5,-.5),vec2(.5,-.5),vec2(0,.5));
    gl_Position=vec4(p[gl_VertexIndex%3],.5,1);
    vout={output_type}({value});
}}
"""
    if stage == "tesc":
        patch_decl = f"layout(location=1) patch out {prefix}{patch[0]} patch_value;" if patch else ""
        patch_store = f"patch_value={prefix}{patch[0]}(vin[0].x+vin[0].y);" if patch else ""
        return header + f"""
layout(vertices=3) out;
layout(location=0) {flat}in {input_type} vin[];
layout(location=0) {flat}out {output_type} vout[];
{patch_decl}
void main() {{
    gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;
    vout[gl_InvocationID]={output_type}({total('[gl_InvocationID]')}/{numeric}(4));
    if(gl_InvocationID==0) {{
        {patch_store}
        gl_TessLevelOuter[0]=1;gl_TessLevelOuter[1]=1;
        gl_TessLevelOuter[2]=1;gl_TessLevelInner[0]=1;
    }}
}}
"""
    if stage == "tese":
        patch_decl = f"layout(location=1) patch in {prefix}{patch[1]} patch_value;" if patch else ""
        patch_load = ""
        if patch:
            values = "+".join("patch_value." + c for c in "xyzw"[:patch[1]])
            patch_load = f"vout+={output_type}({values});"
        return header + f"""
layout(triangles,equal_spacing,ccw) in;
layout(location=0) {flat}in {input_type} vin[];
layout(location=0) {flat}out {output_type} vout;
{patch_decl}
void main() {{
    gl_Position=gl_in[0].gl_Position*gl_TessCoord.x+
                gl_in[1].gl_Position*gl_TessCoord.y+
                gl_in[2].gl_Position*gl_TessCoord.z;
    vout={output_type}(({total('[0]')}+{total('[1]')}+{total('[2]')})/{numeric}(12));
    {patch_load}
}}
"""
    if stage == "geom":
        return header + f"""
layout(triangles) in;
layout(triangle_strip,max_vertices=3) out;
layout(location=0) {flat}in {input_type} vin[];
layout(location=0) {flat}out {output_type} vout;
void main() {{
    for(int i=0;i<3;i++) {{
        gl_Position=gl_in[i].gl_Position;
        vout={output_type}({total('[i]')}/{numeric}(4));
        EmitVertex();
    }}
    EndPrimitive();
}}
"""
    return header + f"""
layout(location=0) {flat}in {input_type} vin;
layout(location=0) out vec4 color;
void main() {{ color=vec4(vec3(float({total()})/256.0),1); }}
"""


class Maintenance4InterfaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        archive = ROOT / "build/libpsbc.host.a"
        cls.glslang = shutil.which("glslangValidator")
        fallback = ROOT / "build/runtime-graphics/toolchain/usr/bin/glslangValidator"
        if not cls.glslang and fallback.is_file():
            cls.glslang = str(fallback)
        siblings = Path(os.environ.get("LAB_SIBLINGS", ROOT.parent))
        gears = siblings / "ps5-agc-gears"
        if not archive.is_file() or not cls.glslang or not (gears / "include").is_dir():
            raise unittest.SkipTest("requires built host PSBC, glslang and graphics headers")
        directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(directory.cleanup)
        cls.tmp = Path(directory.name)
        cls.exe = cls.tmp / "inspect"
        sources = ["tests/maintenance4_interface_inspect.c", "native/runtime_shader.c",
                   "native/runtime_graphics_compiler.c", "src/color_attachment_contract.c",
                   "src/spirv_graphics_interface.c", "src/texture_format.c",
                   "src/ps5_compiler_shims.c"]
        includes = [ROOT / "src", ROOT / "native", gears / "src", gears / "include",
                    ROOT / "third_party/vulkan-headers/include", ROOT / "third_party/psbc-reference"]
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        *["-I" + str(p) for p in includes],
                        *[str(ROOT / p) for p in sources], str(archive),
                        "-lstdc++", "-lm", "-lpthread", "-o", str(cls.exe)],
                       check=True, capture_output=True, text=True)

    def compile_shader(self, stage, source):
        stem = hashlib.sha256(source.encode()).hexdigest()
        path = self.tmp / (stem + "." + stage)
        binary = path.with_suffix(path.suffix + ".spv")
        if not binary.exists():
            path.write_text(source)
            result = subprocess.run([self.glslang, "-V", str(path), "-o", str(binary)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return str(binary)

    def inspect(self, modules, output_width, input_width):
        result = subprocess.run([str(self.exe), *[modules.get(s, "-") for s in STAGES]],
                                capture_output=True, text=True, timeout=60)
        self.assertIn(result.returncode, (0, 1), result.stdout + result.stderr)
        observation = json.loads(result.stdout.strip().splitlines()[-1])
        supported = output_width >= input_width
        self.assertEqual(observation, {
            "enabled": int(supported), "disabled": int(output_width == input_width),
            "result": 0 if supported else -8, "program": int(supported),
            "code": int(supported)}, result.stderr)
        self.assertEqual(result.returncode, 0 if supported else 1)

    def test_stage_boundaries(self):
        for numeric in ("float", "int", "uint"):
            for graph in GRAPHS:
                cases = [(None, 3, 3)]
                cases += [(edge, out_width, in_width) for edge in range(len(graph) - 1)
                          for out_width, in_width in ((4, 3), (4, 2), (3, 2), (2, 3))]
                for edge, out_width, in_width in cases:
                    with self.subTest(numeric=numeric, graph=graph, edge=edge,
                                      widths=(out_width, in_width)):
                        modules = {}
                        for i, stage in enumerate(graph):
                            ins = in_width if edge is not None and i == edge + 1 else 3
                            outs = out_width if edge is not None and i == edge else 3
                            modules[stage] = self.compile_shader(stage, shader(stage, ins, outs, numeric))
                        self.inspect(modules, out_width, in_width)

    def test_patch_boundary(self):
        for numeric in ("float", "int", "uint"):
            for widths in ((3, 3), (4, 3), (4, 2), (3, 2), (2, 3)):
                with self.subTest(numeric=numeric, widths=widths):
                    modules = {stage: self.compile_shader(stage, shader(stage, 3, 3, numeric, widths))
                               for stage in ("vert", "tesc", "tese", "frag")}
                    self.inspect(modules, *widths)


if __name__ == "__main__":
    unittest.main()
