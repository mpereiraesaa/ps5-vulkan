"""Packaging validation, not proof of executing a CTS case."""
from pathlib import Path
import tempfile
import unittest
from tools.embed_cts_reference_images import decode_rgba8, emit

ROOT = Path(__file__).resolve().parents[1]
NAMES = ("patch_vertices_5_in_10_out_ref", "patch_vertices_10_in_5_out_ref",
         "primitive_id_tcs_ref", "primitive_id_tes_ref", "gl_position_ref", "barrier_ref")


class TessReferenceAssets(unittest.TestCase):
    def test_ambiguous_basename_is_rejected_before_decoding(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(SystemExit, "ambiguous"):
                emit(root, root / "out.cpp", [root / "a/same.png", root / "b/same.png"])
            self.assertFalse((root / "out.cpp").exists())

    def test_original_assets_match_independent_decoder(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("independent pixel comparison requires Pillow")
        source = ROOT / "third_party/vk-gl-cts/external/vulkancts/data/vulkan/data/tessellation"
        if not source.is_dir():
            self.skipTest("requires pinned upstream assets")
        builder = (ROOT / "tools/build_upstream_cts.py").read_text()
        for name in NAMES:
            with self.subTest(name=name):
                self.assertIn('"' + name + '"', builder)
                path = source / (name + ".png")
                width, height, pixels = decode_rgba8(path)
                with Image.open(path) as image:
                    self.assertEqual((width, height), image.size)
                    self.assertEqual(pixels, image.convert("RGBA").tobytes())

    def test_isoline_assets_match_independent_decoder(self):
        names = [f"isolines_{spacing}_ref_{level}.png"
                 for spacing in ("equal_spacing", "fractional_even_spacing", "fractional_odd_spacing")
                 for level in range(3)]
        self.assertEqual(len(names), 9)
        self.check_misc_assets(names)

    def test_fill_cover_assets_match_independent_decoder(self):
        self.check_misc_assets([
            f"fill_cover_{primitive}_{spacing}_ref_{level}.png"
            for primitive in ("triangles", "quads")
            for spacing in ("equal_spacing", "fractional_even_spacing", "fractional_odd_spacing")
            for level in range(3)])

    def check_misc_assets(self, names):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("independent pixel comparison requires Pillow")
        source = ROOT / "third_party/vk-gl-cts/external/vulkancts/data/vulkan/data/tessellation"
        if not source.is_dir():
            self.skipTest("requires pinned upstream assets")
        for name in names:
            with self.subTest(name=name):
                width, height, pixels = decode_rgba8(source / name)
                with Image.open(source / name) as image:
                    self.assertEqual((width, height), image.size)
                    self.assertEqual(pixels, image.convert("RGBA").tobytes())


if __name__ == "__main__":
    unittest.main()
