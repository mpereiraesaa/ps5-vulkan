"""Independent point-lattice expectations; not native acceptance evidence."""
from fractions import Fraction
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TessPointOracleTests(unittest.TestCase):
    def test_level_two_quad_maps_to_nine_pixel_centres(self):
        coords = (Fraction(0), Fraction(1, 2), Fraction(1))
        pixels = set()
        for u in coords:
            for v in coords:
                centres = [(c * Fraction(3, 2) - Fraction(47, 64) + 1) * 32
                           for c in (u, v)]
                self.assertTrue(all(c.denominator == 2 for c in centres))
                pixels.add(tuple(int(c) for c in centres))
        self.assertEqual(pixels, {(x, y) for x in (8, 32, 56) for y in (8, 32, 56)})
        self.assertEqual(len(pixels), 9)

    def test_fixture_uses_default_point_size_without_atomic_dependency(self):
        source = (ROOT / "experiments/graphics/runtime_tess_points.tese").read_text()
        self.assertIn("point_mode", source)
        self.assertNotIn("gl_PointSize", source)
        self.assertNotIn("atomic", source)
        self.assertIn("gl_TessCoord.xy*1.5-0.734375", source)

    def test_geometry_transform_is_disjoint_and_retains_domain_colours(self):
        coords = (Fraction(0), Fraction(1, 2), Fraction(1))
        observed = {}
        for u in coords:
            for v in coords:
                x = (u * Fraction(3, 2) - Fraction(47, 64) + Fraction(1, 8) + 1) * 32
                y = (v * Fraction(3, 2) - Fraction(47, 64) - Fraction(1, 8) + 1) * 32
                self.assertEqual(x.denominator, 2)
                self.assertEqual(y.denominator, 2)
                observed[int(x), int(y)] = (v, Fraction(1, 2), u)
        expected = {(x, y) for x in (12, 36, 60) for y in (4, 28, 52)}
        self.assertEqual(set(observed), expected)
        bypass = {(x, y) for x in (8, 32, 56) for y in (8, 32, 56)}
        self.assertFalse(expected & bypass)
        self.assertEqual(len(set(observed.values())), 9)
        source = (ROOT / "experiments/graphics/runtime_tess_points.geom").read_text()
        self.assertIn("gl_in[0].gl_Position+vec4(0.125,-0.125,0,0)", source)
        self.assertIn("domain_color[0].gbr", source)


if __name__ == "__main__":
    unittest.main()
