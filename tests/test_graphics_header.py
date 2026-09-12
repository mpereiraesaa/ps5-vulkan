import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from graphics_header import header_values, validate_user_abi


class HeaderAbiTests(unittest.TestCase):
    def test_texture_user_abi(self):
        def stage(values):
            return dict(user_sgprs=len(values),user_map=values+[0xffffffff]*(32-len(values)))
        gs=stage([0x10000000,0x1000000f,0x10000003,0x10000004])
        validate_user_abi(dict(gs=gs,ps=stage([0x10000000,0])))
        for bad in ([0x10000000,1],[0x10000000,0,1],[0,0x10000000]):
            with self.subTest(bad=bad),self.assertRaises(ValueError):
                validate_user_abi(dict(gs=gs,ps=stage(bad)))
        ps=stage([0x10000000,0]);ps['user_sgprs']=1
        with self.assertRaises(ValueError):validate_user_abi(dict(gs=gs,ps=ps))

    def test_new_user_data_cannot_silently_reuse_procedural_header(self):
        for mapping in ([0x10000000], [0x10000000, 0x10000005, 0x10000004],
                        [0x10000000, 0x10000003, 0x10000004, 0]):
            manifest = {"graphics_requirements": {"stages": {"gs": {
                "user_sgprs": len(mapping), "user_map": mapping + [0xffffffff] * (32 - len(mapping))}}}}
            with self.subTest(mapping=mapping), self.assertRaises(ValueError):
                header_values(manifest)


if __name__ == "__main__":
    unittest.main()
