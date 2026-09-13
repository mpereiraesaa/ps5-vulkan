import struct
import subprocess
import sys
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def module_facts(path: Path):
    payload = path.read_bytes()
    words = struct.unpack(f"<{len(payload) // 4}I", payload)
    capabilities = []
    extensions = []
    cursor = 5
    while cursor < len(words):
        count = words[cursor] >> 16
        opcode = words[cursor] & 0xffff
        if not count or cursor + count > len(words):
            raise ValueError("malformed SPIR-V")
        operands = words[cursor + 1:cursor + count]
        if opcode == 17 and len(operands) == 1:
            capabilities.append(operands[0])
        elif opcode == 10:
            raw = struct.pack(f"<{len(operands)}I", *operands)
            extensions.append(raw.split(b"\0", 1)[0].decode())
        cursor += count
    return words[1], capabilities, extensions


class StorageWidthShaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run([sys.executable, str(ROOT / "tools/prepare_test_shaders.py")],
                       check=True)

    def test_storage8_is_legal_vulkan10_extension_form(self):
        version, capabilities, extensions = module_facts(
            ROOT / "build/test-shaders/storage8.spv")
        self.assertEqual(version, 0x00010000)
        self.assertIn(4448, capabilities)  # StorageBuffer8BitAccess
        self.assertNotIn(4449, capabilities)  # UniformAndStorageBuffer8BitAccess
        self.assertIn("SPV_KHR_8bit_storage", extensions)
        self.assertIn("SPV_KHR_storage_buffer_storage_class", extensions)

    def test_storage16_is_exact_vulkan10_storage_buffer_form(self):
        version, capabilities, extensions = module_facts(
            ROOT / "build/test-shaders/storage16.spv")
        self.assertEqual(version, 0x00010000)
        self.assertIn(4433, capabilities)  # StorageBuffer16BitAccess
        self.assertNotIn(4434, capabilities)  # UniformAndStorageBuffer16BitAccess
        self.assertIn("SPV_KHR_16bit_storage", extensions)


if __name__ == "__main__":
    unittest.main()
