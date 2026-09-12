import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from graphics_upload import plan_graphics_upload


class GraphicsUploadTests(unittest.TestCase):
    def setUp(self):
        self.image = bytes(i % 251 for i in range(848))
        self.sections = [dict(name=".text", offset=0, bytes=768, alignment=256),
                         dict(name=".rodata", offset=768, bytes=80, alignment=16)]
        self.stages = dict(pre_raster=dict(offset=0, bytes=304),
                           fragment=dict(offset=512, bytes=52))
        self.relocs = [dict(offset=128, symbol_offset=768, addend=32, type=1)]

    def test_stages_trailers_constants_and_references(self):
        image, stages, relocs = plan_graphics_upload(self.image, self.sections, self.relocs, self.stages)
        self.assertEqual(image[:304], self.image[:304])
        self.assertEqual(image[304:352], b"barefoot" + bytes(40))
        self.assertEqual(image[512:564], self.image[512:564])
        self.assertEqual(image[564:612], b"barefoot" + bytes(40))
        self.assertEqual(image[624:], self.image[768:])
        self.assertEqual(stages["fragment"], dict(offset=512, isa_bytes=52, shader_bytes=100))
        self.assertEqual(relocs, [dict(offset=128, symbol_offset=656, addend=0, type=1)])

    def test_padding_is_not_an_addressable_symbol_or_patch(self):
        for changes in (dict(symbol_offset=350, addend=0), dict(offset=352), dict(offset=300, type=7)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                plan_graphics_upload(self.image, self.sections, [{**self.relocs[0], **changes}], self.stages)

    def test_overlapping_or_unaligned_stages_rejected(self):
        for offset in (0, 256, 513):
            stages = {**self.stages, "fragment": dict(offset=offset, bytes=52)}
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                plan_graphics_upload(self.image, self.sections, self.relocs, stages)


if __name__ == "__main__":
    unittest.main()
