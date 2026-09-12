import unittest
from tools.check_sampler_uv import check, NUMERATORS


class SamplerUV(unittest.TestCase):
    def fixture(self):
        lines=[]
        for f,n in enumerate(NUMERATORS):
            lines.append(f"1\t2\tMARK\tPS5VK_SAMPLER_PROBE_INPUT frame={f} u_numerator={n} denominator=8192 v_numerator=2048")
            red,green=(373248,0) if n<4088 else (0,373248)
            lines.append(f"1\t2\tMARK\tPS5VK_TEXTURE_READBACK frame={f} red={red} green={green} blue=0 unexpected=0")
        return "\n".join(lines)

    def test_measured_boundary(self):
        self.assertEqual(check(self.fixture())["cases"],13)

    def test_reject_wrong_selection_coverage_or_fixture(self):
        text=self.fixture()
        bad=(text.replace("frame=3 red=0 green=373248","frame=3 red=373248 green=0"),
             text.replace("red=373248","red=373247",1),text.replace("v_numerator=2048","v_numerator=0"),
             text+"\n"+text.splitlines()[0],"\n".join(text.splitlines()[1:]),"")
        for value in bad:
            with self.subTest(value=value),self.assertRaises(ValueError):check(value)


if __name__ == "__main__":
    unittest.main()
