import unittest
from tools.scene_witnesses import witnesses
from tools.check_scene_witnesses import check


class CheckSceneWitnesses(unittest.TestCase):
    def test_depth_disabled_control_discriminates(self):
        rows=self.records();expected=witnesses();control=[]
        for i in (0,1,2,5):
            control.append(rows[i].replace(f"expected_bgra={expected[i]['bgra']:08x}",
                                          f"expected_bgra={expected[i]['off_bgra']:08x}"))
            control.append(f"1\t1\tMARK\tPS5VK_WITNESS_DEPTH index={i} enabled=0")
        self.assertEqual(check("\n".join(control),True)["exact_witnesses"],4)
        with self.assertRaises(ValueError):check("\n".join(control).replace("enabled=0","enabled=1"),True)
        with self.assertRaises(ValueError):check("\n".join(control[:-1]),True)
        # Ignoring depth-off behavior cannot pass the counterfactual oracle.
        control[0]=rows[0]
        with self.assertRaises(ValueError):check("\n".join(control),True)
    def records(self):
        return [f"{i+1}\t{i+1}\tMARK\tPS5VK_SCENE_WITNESS index={i} "
                f"frame={w['frame']} x={w['x']} y={w['y']} owner={w['owner']} "
                f"expected_bgra={w['bgra']:08x} expected_count=1 other=0 changed=1 valid=1"
                for i,w in enumerate(witnesses())]

    def test_exact_results(self):
        self.assertEqual(check("\n".join(self.records()))["exact_witnesses"],6)

    def test_each_missing_result_rejected(self):
        for index in range(6):
            rows=self.records();rows.pop(index)
            with self.assertRaises(ValueError):check("\n".join(rows))

    def test_modified_counts_inputs_and_duplicates(self):
        for old,new in (("expected_count=1","expected_count=0"),("other=0","other=1"),
                ("changed=1","changed=2"),("valid=1","valid=0"),("owner=0","owner=1"),
                ("x=990","x=991"),("expected_bgra=ff00ff00","expected_bgra=ffff0000")):
            rows=self.records();rows[0]=rows[0].replace(old,new)
            with self.subTest(old=old),self.assertRaises(ValueError):check("\n".join(rows))
        rows=self.records();rows.insert(1,rows[0])
        with self.assertRaises(ValueError):check("\n".join(rows))
