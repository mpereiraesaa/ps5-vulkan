import unittest
from tools.scene_reference import box_hit, inverse_rotate, pixel, tile, nearest_index, pixel_without_depth


class SceneReference(unittest.TestCase):
    def test_disabled_depth_uses_last_covering_face(self):
        self.assertEqual(pixel(990,440,0),("green",2,0))
        self.assertEqual(pixel_without_depth(990,440,0),("blue",1))
        self.assertEqual(pixel_without_depth(810,440,45),("red",1))
        self.assertEqual(pixel_without_depth(0,0,0),("black",None))

    def test_optional_quantization_is_not_default(self):
        self.assertEqual(nearest_index(.4995),0)
        self.assertEqual(nearest_index(.4995,8),1)
        self.assertEqual(nearest_index(.498,8),0)
        for value, expected in ((-2,0),(0,0),(.5,1),(1,1),(2,1)):
            self.assertEqual(nearest_index(value),expected)
            self.assertEqual(nearest_index(value,8),expected)
        for bits in (0,3,17,8.5):
            with self.assertRaises(ValueError):nearest_index(.5,bits)

    def test_front_face_ray(self):
        hit=box_hit((0,0,4),(0,0,-1),(0,0,0),1)
        self.assertEqual(hit,(3,.5,.5))

    def test_parallel_miss_and_clipping(self):
        self.assertIsNone(box_hit((2,0,4),(0,0,-1),(0,0,0),1))
        self.assertIsNone(box_hit((0,0,40),(0,0,-1),(0,0,0),1))

    def test_nearest_and_uv_on_side(self):
        self.assertEqual(box_hit((4,0,0),(-1,0,0),(0,0,0),1),(3,.5,.5))

    def test_origin_rotation_preserves_length(self):
        v=inverse_rotate((1,2,3),1.2)
        self.assertAlmostEqual(sum(x*x for x in v),14,places=7)

    def test_background(self):
        for f in (0,45,90,135,179):
            self.assertEqual(pixel(0,0,f),("black",0,None))

    def test_block_extent_and_bounds(self):
        t=tile(0,0,0)
        self.assertEqual(t["counts"],dict(black=16384,red=0,green=0,blue=0))
        for args in ((15,0,0),(0,8,0),(0,0,180),(-1,0,0)):
            with self.assertRaises(ValueError): tile(*args)
