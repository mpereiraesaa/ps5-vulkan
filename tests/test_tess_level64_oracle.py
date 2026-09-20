"""Host oracle checks only; these do not establish hardware level64 support."""
from fractions import Fraction
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# Fixed expected image, independent of shader float arithmetic:
# eight complete atlas rows plus the first point in row nine.
EXPECTED = {(x, y) for y in range(7, 50, 6) for x in range(7, 50, 6)} | {(7, 55)}


def edge_pixels(level):
    result = set()
    for vertex in range(level + 1):
        u = Fraction(vertex, level)
        index = int(u * 64 + Fraction(1, 2))
        result.add((7 + 6 * (index % 8), 7 + 6 * (index // 8)))
    return result


class TessLevel64OracleTests(unittest.TestCase):
    def test_exact_65_pixel_oracle(self):
        self.assertEqual(len(EXPECTED), 65)
        self.assertEqual(edge_pixels(64), EXPECTED)
        self.assertTrue(all(0 <= x < 64 and 0 <= y < 64 for x, y in EXPECTED))

    def test_every_lower_integer_level_fails(self):
        for level in range(1, 64):
            with self.subTest(level=level):
                self.assertNotEqual(edge_pixels(level), EXPECTED)
                self.assertTrue(EXPECTED - edge_pixels(level))
        self.assertEqual(len(EXPECTED - edge_pixels(32)), 32)

    def test_pixel_centres_are_exact_binary_rationals(self):
        for x, y in EXPECTED:
            for coord in (x, y):
                ndc = (Fraction(coord) + Fraction(1, 2)) / 32 - 1
                self.assertEqual((ndc + 1) * 32, Fraction(2 * coord + 1, 2))
                self.assertEqual(Fraction(float(ndc)), ndc)

    def test_own_sources_compile(self):
        compiler = shutil.which('glslangValidator')
        if not compiler:
            self.skipTest('glslangValidator unavailable; native validation still required')
        with tempfile.TemporaryDirectory() as directory:
            for stage in ('tesc', 'tese'):
                source = ROOT / f'experiments/graphics/runtime_tess_level64.{stage}'
                subprocess.run([compiler, '-V', '--target-env', 'vulkan1.0',
                                str(source), '-o', str(Path(directory) / f'{stage}.spv')],
                               check=True, capture_output=True, text=True)


if __name__ == '__main__':
    unittest.main()
