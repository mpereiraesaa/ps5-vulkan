import unittest
from tools.check_planar_scissors import check


class PlanarScissors(unittest.TestCase):
    def fixture(self):
        regions = ((0, 0, 960, 540, 139968), (960, 0, 960, 540, 139968),
                   (0, 540, 960, 540, 46656), (960, 540, 960, 540, 46656),
                   (0, 0, 1920, 1080, 373248))
        return "\n".join(f"1\t2\tMARK\tPS5VK_SCISSOR_PROBE mode=3 depth={d} x={x} y={y} width={w} height={h} total_changed={n}"
                         for d in (0, 1) for x, y, w, h, n in regions)

    def test_exact_partition(self):
        self.assertEqual(check(self.fixture())["cases"], 10)

    def test_old_stale_readback_is_rejected(self):
        with self.assertRaises(ValueError):
            check(self.fixture().replace("total_changed=139968", "total_changed=30976", 1))

    def test_missing_duplicate_and_wrong_fixture(self):
        text = self.fixture()
        for bad in ("", "\n".join(text.splitlines()[1:]), text + "\n" + text.splitlines()[0],
                    text.replace("mode=3", "mode=2"), text.replace("depth=1", "depth=7")):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                check(bad)


if __name__ == "__main__":
    unittest.main()
