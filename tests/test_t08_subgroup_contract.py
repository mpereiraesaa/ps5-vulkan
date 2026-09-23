"""Source-derived T08 subgroup gates and a real PSBC compile boundary."""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "third_party/vulkan-headers/registry/vk.xml"
CTS = ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/vulkan/subgroups"


def instructions(payload):
    words = struct.unpack(f"<{len(payload) // 4}I", payload)
    assert words[0] == 0x07230203
    offset = 5
    while offset < len(words):
        count, opcode = words[offset] >> 16, words[offset] & 0xffff
        if not count or offset + count > len(words):
            raise AssertionError("malformed SPIR-V")
        yield opcode, words[offset + 1:offset + count]
        offset += count


class T08SubgroupContracts(unittest.TestCase):
    def test_pinned_registry_routes(self):
        if not REGISTRY.is_file():
            self.skipTest("pinned Vulkan registry unavailable")
        root = ET.parse(REGISTRY).getroot()
        extensions = {item.get("name"): item for item in root.findall(".//extensions/extension")}
        self.assertIn("VK_VERSION_1_1",
                      extensions["VK_KHR_shader_subgroup_extended_types"].get("depends"))
        self.assertFalse(any("subgroupBroadcastDynamicId" in
                             ET.tostring(item, encoding="unicode")
                             for item in extensions.values()))
        versions = [feature.get("name") for feature in root.findall("feature")
                    if any(member.get("name") == "subgroupBroadcastDynamicId"
                           for member in feature.findall(".//feature"))]
        self.assertEqual(versions, ["VK_VERSION_1_2"])

    def test_original_cts_eligibility(self):
        source = CTS / "vktSubgroupsTestsUtils.cpp"
        broadcast = CTS / "vktSubgroupsBallotBroadcastTests.cpp"
        if not source.is_file() or not broadcast.is_file():
            self.skipTest("pinned upstream CTS unavailable")
        utilities = source.read_text()
        factory = broadcast.read_text()
        self.assertRegex(utilities, r"isSubgroupBroadcastDynamicIdSupported\(Context &context\)")
        self.assertRegex(utilities, r"contextSupports\(vk::ApiVersion\(0, 1, 2, 0\)\)")
        self.assertIn("getPhysicalDeviceVulkan12Features", utilities)
        self.assertIn("shaderSubgroupExtendedTypes && shaderInt8", utilities)
        self.assertIn("shaderSubgroupExtendedTypes && shaderInt16", utilities)
        self.assertIn("shaderSubgroupExtendedTypes && shaderInt64", utilities)
        self.assertIn("shaderSubgroupExtendedTypes && shaderFloat16", utilities)
        self.assertIn("subgroupbroadcast_nonconst", factory)
        self.assertIn("isSubgroupBroadcastDynamicIdSupported(context)", factory)

    def test_runtime_id_and_narrow_type_compiler_gate(self):
        glslang = shutil.which("glslangValidator")
        archive = ROOT / "build/libpsbc.host.a"
        if not glslang or not archive.is_file():
            self.skipTest("host PSBC archive and glslangValidator required")
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            probe = temp / "probe"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Ithird_party/psbc-reference/libpsbc",
                            "tests/t08_compile_probe.c", str(archive),
                            "-lstdc++", "-lm", "-lpthread", "-o", str(probe)],
                           cwd=ROOT, check=True, capture_output=True, text=True)
            header = """#version 450
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) buffer Data { uint values[]; } data;
"""
            shaders = {
                "runtime": header + """void main() {
 uint lane = gl_SubgroupInvocationID;
 uint source = data.values[gl_WorkGroupID.x * 65u + 64u];
 uint value = gl_WorkGroupID.x * 1000u + lane;
 data.values[gl_GlobalInvocationID.x] = subgroupBroadcast(value, source);
}
""",
                "int16": header.replace("layout(local_size_x=64)",
                    "#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require\n"
                    "#extension GL_EXT_shader_subgroup_extended_types_int16 : require\n"
                    "layout(local_size_x=64)") + """void main() {
 int16_t value = int16_t(gl_SubgroupInvocationID + gl_WorkGroupID.x * 31u);
 data.values[gl_GlobalInvocationID.x] = uint(subgroupBroadcast(value, 1u));
}
""",
            }
            for name, source in shaders.items():
                shader = temp / f"{name}.comp"
                binary = temp / f"{name}.spv"
                shader.write_text(source)
                subprocess.run([glslang, "-V", "--target-env", "vulkan1.2",
                                str(shader), "-o", str(binary)], check=True,
                               capture_output=True, text=True)
                payload = binary.read_bytes()
                self.assertEqual(struct.unpack_from("<I", payload, 4)[0], 0x00010500)
                self.assertIn(337, [opcode for opcode, _ in instructions(payload)])
                if name == "int16":
                    capabilities = [args[0] for opcode, args in instructions(payload)
                                    if opcode == 17]
                    self.assertIn(22, capabilities)
                    for enabled, expected in (("none", "result=7"),
                                              ("int16", "result=0")):
                        result = subprocess.run([str(probe), "subgroup", str(binary),
                                                 enabled], capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertIn(expected, result.stdout)
                else:
                    result = subprocess.run([str(probe), "subgroup", str(binary),
                                             "none"], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("result=0", result.stdout)
                    self.assertRegex(result.stdout, r"code_bytes=[1-9][0-9]*")

    def test_graphics_broadcast_survives_pipeline_context(self):
        """A compiler success is meaningful only if Broadcast changes live ISA."""
        glslang = shutil.which("glslangValidator")
        archive = ROOT / "build/libpsbc.host.a"
        if not glslang or not archive.is_file():
            self.skipTest("host PSBC archive and glslangValidator required")
        header = """#version 450
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
layout(set=0,binding=0,std430) readonly buffer Sources { uint ids[]; } data;
"""
        sources = {
            "vertex": ("vert", header + """layout(location=0) flat out uint result;
void main() {
    uint source = data.ids[gl_VertexIndex & 3];
    result = subgroupBroadcast(gl_SubgroupInvocationID, source);
    gl_Position = vec4(float(gl_VertexIndex & 1) * 0.5, 0.0, 0.0, 1.0);
}
"""),
            "fragment": ("frag", header + """layout(location=0) out vec4 color;
void main() {
    uint source = data.ids[uint(gl_FragCoord.x) & 3u];
    uint result = subgroupBroadcast(gl_SubgroupInvocationID, source);
    color = vec4(float(result) / 32.0, 0.0, 0.0, 1.0);
}
"""),
        }
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            probe = temp / "probe"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Ithird_party/psbc-reference/libpsbc",
                            "tests/t08_graphics_subgroup_probe.c", str(archive),
                            "-lstdc++", "-lm", "-lpthread", "-o", str(probe)],
                           cwd=ROOT, check=True, capture_output=True, text=True)
            for stage, (suffix, source) in sources.items():
                signatures = {}
                for variant, shader_source in (
                    ("broadcast", source),
                    ("control", source.replace(
                        "subgroupBroadcast(gl_SubgroupInvocationID, source)",
                        "gl_SubgroupInvocationID")),
                ):
                    shader = temp / f"{stage}-{variant}.{suffix}"
                    binary = temp / f"{stage}-{variant}.spv"
                    shader.write_text(shader_source)
                    subprocess.run([glslang, "-V", "--target-env", "vulkan1.2",
                                    str(shader), "-o", str(binary)], check=True,
                                   capture_output=True, text=True)
                    payload = binary.read_bytes()
                    ops = list(instructions(payload))
                    broadcasts = [args for opcode, args in ops if opcode == 337]
                    self.assertEqual(len(broadcasts), int(variant == "broadcast"))
                    if broadcasts:
                        loads = {args[1] for opcode, args in ops if opcode == 61}
                        self.assertIn(broadcasts[0][-1], loads)
                    result = subprocess.run([str(probe), stage, str(binary)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    match = re.fullmatch(
                        r"result=0 code_bytes=([1-9][0-9]*) descriptors=1 "
                        r"fnv64=([0-9a-f]{16})\n", result.stdout)
                    self.assertIsNotNone(match, result.stdout)
                    signatures[variant] = match.group(2)
                self.assertNotEqual(signatures["broadcast"], signatures["control"],
                                    f"{stage} Broadcast was discarded by the compiler")

    def test_signed_int8_graphics_broadcast_survives_pipeline_context(self):
        """A runtime ID and signed narrow result survive both graphics stages."""
        glslang = shutil.which("glslangValidator")
        archive = ROOT / "build/libpsbc.host.a"
        if not glslang or not archive.is_file():
            self.skipTest("host PSBC archive and glslangValidator required")
        header = """#version 450
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_subgroup_extended_types_int8 : require
layout(set=0,binding=0,std430) readonly buffer Sources { uint ids[]; } data;
"""
        sources = {
            "vertex": ("vert", header + """layout(location=0) flat out uint result;
void main() {
    uint source = data.ids[gl_VertexIndex & 3];
    int8_t item = int8_t(int(gl_SubgroupInvocationID) - 16);
    result = uint(subgroupBroadcast(item, source));
    gl_Position = vec4(float(gl_VertexIndex & 1) * 0.5, 0.0, 0.0, 1.0);
}
"""),
            "fragment": ("frag", header + """layout(location=0) out vec4 color;
void main() {
    uint source = data.ids[uint(gl_FragCoord.x) & 3u];
    int8_t item = int8_t(int(gl_SubgroupInvocationID) - 16);
    uint result = uint(subgroupBroadcast(item, source));
    color = vec4(float(result & 255u) / 255.0, 0.0, 0.0, 1.0);
}
"""),
        }
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            probe = temp / "probe"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Ithird_party/psbc-reference/libpsbc",
                            "tests/t08_graphics_subgroup_probe.c", str(archive),
                            "-lstdc++", "-lm", "-lpthread", "-o", str(probe)],
                           cwd=ROOT, check=True, capture_output=True, text=True)
            for stage, (suffix, source) in sources.items():
                signatures = {}
                for variant, shader_source in (
                    ("broadcast", source),
                    ("control", source.replace(
                        "uint(subgroupBroadcast(item, source))",
                        "uint(item) + source")),
                ):
                    shader = temp / f"{stage}-{variant}.{suffix}"
                    binary = temp / f"{stage}-{variant}.spv"
                    shader.write_text(shader_source)
                    subprocess.run([glslang, "-V", "--target-env", "vulkan1.2",
                                    str(shader), "-o", str(binary)], check=True,
                                   capture_output=True, text=True)
                    ops = list(instructions(binary.read_bytes()))
                    broadcasts = [args for opcode, args in ops if opcode == 337]
                    self.assertEqual(len(broadcasts), int(variant == "broadcast"))
                    if broadcasts:
                        signed_int8 = {args[0] for opcode, args in ops
                                       if opcode == 21 and args[1:] == (8, 1)}
                        loads = {args[1] for opcode, args in ops if opcode == 61}
                        self.assertIn(broadcasts[0][0], signed_int8)
                        self.assertIn(broadcasts[0][-1], loads)
                    result = subprocess.run([str(probe), stage, str(binary), "int8"],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    match = re.fullmatch(
                        r"result=0 code_bytes=([1-9][0-9]*) descriptors=1 "
                        r"fnv64=([0-9a-f]{16})\n", result.stdout)
                    self.assertIsNotNone(match, result.stdout)
                    signatures[variant] = match.group(2)
                self.assertNotEqual(signatures["broadcast"], signatures["control"],
                                    f"{stage} signed Int8 Broadcast was discarded")

    def test_extended_arithmetic_lowering(self):
        """Original CTS arithmetic reductions and scans retain live PSBC ISA."""
        glslang = shutil.which("glslangValidator")
        archive = ROOT / "build/libpsbc.host.a"
        factory = CTS / "vktSubgroupsArithmeticTests.cpp"
        utilities = CTS / "vktSubgroupsTestsUtils.cpp"
        if not all((glslang, archive.is_file(), factory.is_file(),
                    utilities.is_file())):
            self.skipTest("pinned CTS, host PSBC archive and glslang required")
        factory_source = factory.read_text()
        self.assertIn("createSubgroupsArithmeticTests", factory_source)
        self.assertIn("subgroups::getAllFormats()", factory_source)
        operation_names = ("Add", "Mul", "Min", "Max", "And", "Or", "Xor")
        for name in operation_names:
            for prefix in ("", "INCLUSIVE_", "EXCLUSIVE_"):
                self.assertIn(f"OPTYPE_{prefix}{name.upper()}", factory_source)
        self.assertIn("if (isFloat && isBitwiseOp)", factory_source)
        format_source = utilities.read_text()
        self.assertIn("getAllFormats()", format_source)
        formats = {
            "int8": ("uint8_t", "u8vec", "GL_EXT_shader_explicit_arithmetic_types_int8",
                     "GL_EXT_shader_subgroup_extended_types_int8"),
            "int8_signed": ("int8_t", "i8vec", "GL_EXT_shader_explicit_arithmetic_types_int8",
                            "GL_EXT_shader_subgroup_extended_types_int8"),
            "int16": ("int16_t", "i16vec", "GL_EXT_shader_explicit_arithmetic_types_int16",
                      "GL_EXT_shader_subgroup_extended_types_int16"),
            "int16_unsigned": ("uint16_t", "u16vec", "GL_EXT_shader_explicit_arithmetic_types_int16",
                               "GL_EXT_shader_subgroup_extended_types_int16"),
            "int64": ("uint64_t", "u64vec", "GL_ARB_gpu_shader_int64",
                      "GL_EXT_shader_subgroup_extended_types_int64"),
            "int64_signed": ("int64_t", "i64vec", "GL_ARB_gpu_shader_int64",
                             "GL_EXT_shader_subgroup_extended_types_int64"),
            "float16": ("float16_t", "f16vec", "GL_EXT_shader_explicit_arithmetic_types_float16",
                        "GL_EXT_shader_subgroup_extended_types_float16"),
        }
        opcodes = {
            "int8": (349, 351, 354, 357, 359, 360, 361),
            "int8_signed": (349, 351, 353, 356, 359, 360, 361),
            "int16": (349, 351, 353, 356, 359, 360, 361),
            "int16_unsigned": (349, 351, 354, 357, 359, 360, 361),
            "int64": (349, 351, 354, 357, 359, 360, 361),
            "int64_signed": (349, 351, 353, 356, 359, 360, 361),
            "float16": (350, 352, 355, 358),
        }
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            probe = temp / "probe"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Ithird_party/psbc-reference/libpsbc",
                            "tests/t08_compile_probe.c", str(archive),
                            "-lstdc++", "-lm", "-lpthread", "-o", str(probe)],
                           cwd=ROOT, check=True, capture_output=True, text=True)
            for kind, (scalar, vector, explicit, extended) in formats.items():
                for width in (1, 2, 3, 4):
                    with self.subTest(kind=kind, width=width):
                        channels = ("R", "RG", "RGB", "RGBA")[width - 1]
                        bits, suffix = {
                            "int8": (8, "UINT"), "int8_signed": (8, "SINT"),
                            "int16": (16, "SINT"), "int16_unsigned": (16, "UINT"),
                            "int64": (64, "UINT"), "int64_signed": (64, "SINT"),
                            "float16": (16, "SFLOAT"),
                        }[kind]
                        format_name = "".join(f"{channel}{bits}" for channel in channels)
                        self.assertIn(f"formats.push_back(VK_FORMAT_{format_name}_{suffix})",
                                      format_source)
                        typename = scalar if width == 1 else f"{vector}{width}"
                        components = ", ".join(
                            f"{scalar}(gl_SubgroupInvocationID + {index}u)"
                            for index in range(width))
                        expression = f"{typename}({components})"
                        header = ("#version 450\n"
                                  "#extension GL_KHR_shader_subgroup_arithmetic : require\n"
                                  f"#extension {explicit} : require\n"
                                  f"#extension {extended} : require\n"
                                  "layout(local_size_x=64) in;\n"
                                  "layout(set=0,binding=0,std430) buffer Data "
                                  "{ uint values[]; } data;\n")
                        signatures = {}
                        supported = operation_names[:4] if kind == "float16" else operation_names
                        variants = [("control", "value", None, None)]
                        for name, opcode in zip(supported, opcodes[kind]):
                            for mode, prefix, group_operation in (
                                ("reduce", "", 0), ("inclusive", "Inclusive", 1),
                                ("exclusive", "Exclusive", 2)):
                                variants.append((f"{mode}-{name.lower()}",
                                                 f"subgroup{prefix}{name}(value)",
                                                 opcode, group_operation))
                        for variant, operation, opcode, group_operation in variants:
                            outputs = "\n".join(
                                f"data.values[gl_GlobalInvocationID.x * {width}u + {index}u]"
                                f" = uint(result{'.' + 'xyzw'[index] if width > 1 else ''});"
                                for index in range(width))
                            shader = temp / f"{kind}-vec{width}-{variant}.comp"
                            binary = shader.with_suffix(".spv")
                            shader.write_text(
                                header + "void main() {\n"
                                f"  {typename} value = {expression};\n"
                                f"  {typename} result = {operation};\n"
                                f"  {outputs}\n}}\n")
                            subprocess.run([glslang, "-V", "--target-env", "vulkan1.2",
                                            str(shader), "-o", str(binary)], check=True,
                                           capture_output=True, text=True)
                            arithmetic = [(op, args[3]) for op, args in
                                          instructions(binary.read_bytes())
                                          if 349 <= op <= 361]
                            expected = [] if opcode is None else [(opcode, group_operation)]
                            self.assertEqual(arithmetic, expected)
                            result = subprocess.run(
                                [str(probe), "subgroup", str(binary),
                                 kind.split("_", 1)[0]],
                                capture_output=True, text=True)
                            self.assertEqual(result.returncode, 0, result.stderr)
                            match = re.fullmatch(
                                r"result=0 code_bytes=[1-9][0-9]* descriptors=1 "
                                r"fnv64=([0-9a-f]{16})\n", result.stdout)
                            self.assertIsNotNone(match, result.stdout)
                            signatures[variant] = match.group(1)
                        self.assertEqual(len(set(signatures.values())), len(signatures),
                                         "CTS arithmetic operations must have distinct live ISA")


if __name__ == "__main__":
    unittest.main()
