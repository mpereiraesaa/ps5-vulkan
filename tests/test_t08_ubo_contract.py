"""Pinned standard-layout route and compact UBO compiler fixture."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "third_party/vulkan-headers/registry/vk.xml"
CTS = ROOT / "third_party/vk-gl-cts/external/vulkancts/modules/vulkan/ubo"


class T08UniformBufferContracts(unittest.TestCase):
    def test_shipping_feature_retires_measurement_switch(self):
        source = (ROOT / "native/platform_ps5.c").read_text()
        self.assertIn("PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT", source)
        for relative in ("native/platform_ps5.c", "tools/build_sdk.py",
                         "tools/build_upstream_cts.py"):
            self.assertNotIn("PS5VK_UBO_STANDARD_LAYOUT_DIAGNOSTIC",
                             (ROOT / relative).read_text())

    def test_pinned_extension_and_original_factory(self):
        if not REGISTRY.is_file():
            self.skipTest("pinned Vulkan registry unavailable")
        root = ET.parse(REGISTRY).getroot()
        extension = root.find(".//extensions/extension[@name='VK_KHR_uniform_buffer_standard_layout']")
        self.assertIsNotNone(extension)
        # In vk.xml dependency expressions a comma is OR: this extension has
        # a legal Vulkan 1.0 route through the properties2 instance extension.
        self.assertEqual(extension.get("depends").split(","), [
            "VK_KHR_get_physical_device_properties2", "VK_VERSION_1_1"])
        self.assertIsNotNone(extension.find(
            ".//feature[@name='uniformBufferStandardLayout']"))
        case = CTS / "vktUniformBlockCase.cpp"
        factory = CTS / "vktUniformBlockTests.cpp"
        if not case.is_file() or not factory.is_file():
            self.skipTest("pinned upstream CTS unavailable")
        self.assertIn("FLAG_ALLOW_STD430_UBOS", case.read_text())
        self.assertIn("getUniformBufferStandardLayoutFeatures().uniformBufferStandardLayout",
                      case.read_text())
        self.assertIn('{"std430", LAYOUT_STD430}', factory.read_text())

    def test_std430_member_array_and_matrix_strides_compile(self):
        glslang = shutil.which("glslangValidator")
        archive = ROOT / "build/libpsbc.host.a"
        if not glslang or not archive.is_file():
            self.skipTest("host PSBC archive and glslangValidator required")
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            shader = temp / "standard.comp"
            binary = temp / "standard.spv"
            probe = temp / "probe"
            shader.write_text("""#version 450
#extension GL_EXT_scalar_block_layout : require
layout(local_size_x=1) in;
layout(std430,set=0,binding=0) uniform Input {
 uint a[3];
 vec2 b[2];
 mat2 c;
} u;
layout(std430,set=0,binding=1) buffer Output { uint values[]; } outputData;
void main() {
 uint i = gl_GlobalInvocationID.x;
 outputData.values[i] = u.a[i % 3u] +
     floatBitsToUint(u.b[i % 2u].x) + floatBitsToUint(u.c[0].x);
}
""")
            subprocess.run([glslang, "-V", "--target-env", "vulkan1.2",
                            str(shader), "-o", str(binary)], check=True,
                           capture_output=True, text=True)
            payload = binary.read_bytes()
            words = struct.unpack(f"<{len(payload) // 4}I", payload)
            decorations = []
            offset = 5
            while offset < len(words):
                count, opcode = words[offset] >> 16, words[offset] & 0xffff
                self.assertGreater(count, 0)
                self.assertLessEqual(offset + count, len(words))
                if opcode in (71, 72):
                    decorations.append((opcode, words[offset + 1:offset + count]))
                offset += count
            array_strides = [args[2] for opcode, args in decorations
                             if opcode == 71 and len(args) >= 3 and args[1] == 6]
            member_offsets = [args[3] for opcode, args in decorations
                              if opcode == 72 and len(args) >= 4 and args[2] == 35]
            matrix_strides = [args[3] for opcode, args in decorations
                              if opcode == 72 and len(args) >= 4 and args[2] == 7]
            self.assertIn(4, array_strides)
            self.assertIn(8, array_strides)
            self.assertTrue({0, 16, 32}.issubset(member_offsets))
            self.assertIn(8, matrix_strides)

            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Ithird_party/psbc-reference/libpsbc",
                            "tests/t08_compile_probe.c", str(archive),
                            "-lstdc++", "-lm", "-lpthread", "-o", str(probe)],
                           cwd=ROOT, check=True, capture_output=True, text=True)
            result = subprocess.run([str(probe), "ubo", str(binary), "none"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("result=0", result.stdout)
            self.assertIn("descriptors=2", result.stdout)


if __name__ == "__main__":
    unittest.main()
