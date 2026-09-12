import unittest
from unittest.mock import patch
from tools.scene_witnesses import witnesses, header, POINTS


class SceneWitnessTests(unittest.TestCase):
    def test_independent_interior_overlap(self):
        rows=witnesses()
        self.assertEqual(len(rows),6)
        self.assertEqual({r["owner"] for r in rows},{0,1})
        self.assertEqual({r["color"] for r in rows},{"red","green","blue"})
        self.assertEqual({r["frame"] for r in rows},{0,45,90,135})
        self.assertEqual(len({(r["frame"],r["x"],r["y"]) for r in rows}),6)
        self.assertEqual(header().count("UINT32_C("),12)
        self.assertEqual([rows[i]["off_bgra"]!=rows[i]["bgra"] for i in (0,1,2,5)],
                         [True,True,False,True])

    def test_nonoverlap_or_wrong_owner_rejected(self):
        for returned in (("green",1,0),("green",2,1),("black",0,None)):
            with patch("tools.scene_witnesses.pixel",return_value=returned):
                with self.assertRaises(ValueError):witnesses()

    def test_single_bad_neighborhood_sample_rejected(self):
        from tools.scene_reference import pixel
        frame,x,y,_,_=POINTS[0]
        def perturbed(px,py,pf,precision):
            if (px,py,pf)==(x+3,y-2,frame):return "black",0,None
            return pixel(px,py,pf,precision)
        with patch("tools.scene_witnesses.pixel",side_effect=perturbed):
            with self.assertRaises(ValueError):witnesses()
