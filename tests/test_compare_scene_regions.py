import unittest
from tools.compare_scene_regions import EXPECTED, parse_regions


class RegionInput(unittest.TestCase):
    def setUp(self):
        self.lines=[f"{n}\t{n}\tMARK\tPS5VK_SCENE_REGION frame={f} tile_x={x} tile_y={y} black=16384 red=0 green=0 blue=0 unexpected=0"
                    for n,(f,x,y) in enumerate(sorted(EXPECTED),1)]

    def test_complete(self):
        self.assertEqual(set(parse_regions("\n".join(self.lines))),EXPECTED)

    def test_missing_duplicate_bad_count_unknown(self):
        variants=[self.lines[:-1],self.lines+[self.lines[0]],
                  [self.lines[0].replace("black=16384","black=16383")]+self.lines[1:],
                  [self.lines[0].replace("unexpected=0","unexpected=1")]+self.lines[1:]]
        for lines in variants:
            with self.assertRaises(ValueError):parse_regions("\n".join(lines))
