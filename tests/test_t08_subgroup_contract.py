"""Source-derived T08 subgroup gates and a real PSBC compile boundary."""
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
