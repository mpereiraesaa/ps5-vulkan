import struct
import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from graphics_interfaces import inventory


def inst(op, *args):
    return [(len(args)+1)<<16 | op, *args]


def sample(extra=()):
    words = [0x07230203, 0x10000, 0, 20, 0]
    words += inst(15, 0, 1, 0x6e69616d, 0, 8)
    words += inst(22, 2, 32) + inst(23, 3, 2, 4)
    words += inst(30, 4, 3) + inst(71, 4, 2) + inst(72, 4, 0, 11, 0)
    words += inst(32, 7, 3, 4) + inst(59, 7, 8, 3) + list(extra)
    return struct.pack('<'+'I'*len(words), *words)


class InterfaceTests(unittest.TestCase):
    def test_builtin_members_are_not_dropped(self):
        result = inventory(sample())
        block = result['declared_interfaces'][0]['type']['element']
        self.assertEqual(block['members'][0]['decorations'], [[11, 0]])
        self.assertEqual(block['members'][0]['type']['count'], 4)
        self.assertFalse(result['limit_validation'])
        self.assertFalse(result['static_usage_analysis'])

    def test_location_preserved_separately(self):
        result = inventory(sample(inst(71, 8, 30, 5)))
        self.assertEqual(result['declared_interfaces'][0]['decorations'], [[30, 5]])

    def test_bad_declaration_or_type_rejected(self):
        for extra in (inst(32, 7), inst(32, 7, 3, 19), inst(32, 7, 3, 7)):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                inventory(sample(extra))
