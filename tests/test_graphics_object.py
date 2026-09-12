import struct
import unittest
from tools.graphics_object import unpack_graphics_object


def fixture():
    names = b"\0.strtab\0.text\0.rodata\0.symtab\0.rel.text\0"
    data = bytearray(64 + 6 * 64)
    data[:16] = b"\x7fELF\x02\x01\x01\x41" + bytes(8)
    struct.pack_into("<HHIQQQIHHHHHH", data, 16, 1, 224, 1, 0, 0, 64, 0x42, 64, 0, 0, 64, 6, 1)
    def section(index, name, kind, flags, payload, link=0, info=0, alignment=1, stride=0):
        offset = len(data)
        data.extend(payload)
        struct.pack_into("<IIQQQQIIQQ", data, 64 + index * 64,
                         names.index(name.encode()), kind, flags, 0, offset, len(payload),
                         link, info, alignment, stride)
        return offset
    section(1, ".strtab", 3, 0, names)
    text = section(2, ".text", 1, 6, struct.pack("<IIII", 12, 12, 0, 0), alignment=256)
    section(3, ".rodata", 1, 2, bytes(range(64)), alignment=16)
    symbols = bytes(24) + struct.pack("<IBBHQQ", 0, 3, 0, 3, 0, 0)
    symbol_offset = section(4, ".symtab", 2, 0, symbols, link=1, alignment=8, stride=24)
    relocations = struct.pack("<QQQQ", 0, (1 << 32) | 1, 4, (1 << 32) | 2)
    rel = section(5, ".rel.text", 9, 0x40, relocations, link=4, info=2, alignment=8, stride=16)
    return data, text, symbol_offset, rel


class GraphicsObjectTests(unittest.TestCase):
    def test_packing_and_addends(self):
        data, *_ = fixture(); original = bytes(data)
        image, sections, relocations = unpack_graphics_object(data)
        self.assertEqual(sections[1]["offset"], 16)
        self.assertEqual(image[16:], bytes(range(64)))
        self.assertEqual(relocations, [dict(offset=0, symbol_offset=16, addend=12, type=1),
                                      dict(offset=4, symbol_offset=16, addend=12, type=2)])
        self.assertEqual(data, original)

    def test_truncation_and_wrong_target(self):
        data, *_ = fixture()
        for size in (0, 16, 63, 100, len(data) - 1):
            with self.subTest(size=size), self.assertRaises(ValueError):
                unpack_graphics_object(data[:size])
        data[48] = 0x36
        with self.assertRaises(ValueError):
            unpack_graphics_object(data)

    def test_unsupported_and_bad_relocations(self):
        for mode in ("type", "symbol", "target", "duplicate", "addend", "external"):
            data, text, symbols, rel = fixture()
            if mode == "type": struct.pack_into("<Q", data, rel + 8, (1 << 32) | 3)
            if mode == "symbol": struct.pack_into("<Q", data, rel + 8, (9 << 32) | 1)
            if mode == "target": struct.pack_into("<Q", data, rel, 16)
            if mode == "duplicate": struct.pack_into("<Q", data, rel + 16, 0)
            if mode == "addend": struct.pack_into("<I", data, text, 0xffffffff)
            if mode == "external": struct.pack_into("<H", data, symbols + 24 + 6, 0)
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                unpack_graphics_object(data)
