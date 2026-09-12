import struct
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from graphics_spirv import graphics_entry


def module(model):
    return [0x07230203, 0x10000, 0, 2, 0, (5 << 16) | 15, model, 1, 0x6e69616d, 0]


def binary(words):
    return struct.pack(f"<{len(words)}I", *words)


class GraphicsSpirvTests(unittest.TestCase):
    def test_models(self):
        self.assertEqual(graphics_entry(binary(module(0))), "vertex")
        self.assertEqual(graphics_entry(binary(module(4))), "fragment")
        with self.assertRaises(ValueError):
            graphics_entry(binary(module(5)))

    def test_invalid_structure_entries_and_strings(self):
        cases = [module(0)[:-1], module(0) + module(4)[5:]]
        for index, value in ((0, 0), (3, 0), (5, 15), (7, 2), (9, 0xffffffff)):
            words = module(0); words[index] = value; cases.append(words)
        for words in cases:
            with self.subTest(words=words), self.assertRaises(ValueError):
                graphics_entry(binary(words))


if __name__ == "__main__":
    unittest.main()
