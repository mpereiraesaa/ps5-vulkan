"""Regression for the linear coordinate witness, not a GPU substitute."""
import math
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TessCoordinateOracleTests(unittest.TestCase):
    def test_control_exports_unquantized_coordinates(self):
        source = (ROOT / "experiments/graphics/runtime_tess_coord.tese").read_text()
        self.assertIn("out_color = vec3(gl_TessCoord.x, gl_TessCoord.y, 0.5)", source)
        self.assertNotIn("floor(", source)

    def test_interior_coordinate_breaks_old_quantization_assumption(self):
        # Triangle tessellation can generate interior vertices. The old
        # assumption that every component was in {0, .5, 1} was invalid.
        center = 1.0 / 3.0
        self.assertNotAlmostEqual(math.floor(center * 2.0) / 2.0, center)

    def test_linear_oracle_is_independent_of_subtriangle(self):
        vertices = ((0., 0.), (.5, 0.), (1./3., 1./3.))
        weights = (.2, .3, .5)
        uv = [sum(w * v[c] for w, v in zip(weights, vertices)) for c in (0, 1)]
        position = [sum(w * (v[c] * 1.8 - .9) for w, v in zip(weights, vertices))
                    for c in (0, 1)]
        for c in (0, 1):
            self.assertAlmostEqual((position[c] + .9) / 1.8, uv[c])


if __name__ == "__main__":
    unittest.main()
