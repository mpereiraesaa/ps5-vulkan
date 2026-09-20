"""Independent rational-coordinate regression; not hardware acceptance."""
import ctypes
from fractions import Fraction as F
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class TessPointMatrixTests(unittest.TestCase):
    def test_all_domains_and_spacing(self):
        with tempfile.TemporaryDirectory() as directory:
            lib=Path(directory)/"oracle.so"
            subprocess.run(["cc","-shared","-fPIC","-Wall","-Werror","-Inative",
                            "-x","c","-","-o",str(lib)],cwd=ROOT,check=True,
                input='#include "tess_point_matrix.h"\n'
                      'int inside(unsigned d,unsigned s,unsigned x,unsigned y) '
                      '{return ps5vk_tess_point_inside(d,s,x,y);}\n'
                      'unsigned count(unsigned d,unsigned s) '
                      '{return ps5vk_tess_point_count(d,s);}\n',text=True)
            oracle=ctypes.CDLL(str(lib))
            for domain in range(3):
                for spacing in range(3):
                    n=3 if spacing==2 else 2
                    if domain==0:
                        coords={(F(0),F(i,n)) for i in range(n+1)}
                        coords|={(F(i,n),F(0)) for i in range(n+1)}
                        coords|={(F(i,n),F(n-i,n)) for i in range(n+1)}
                        coords|=({(F(1,3),F(1,3))} if n==2 else
                                  {(F(5,9),F(2,9)),(F(2,9),F(5,9)),(F(2,9),F(2,9))})
                    else:
                        coords={(F(x,n),F(y,n)) for x in range(n+1)
                                for y in range(n+1 if domain==1 else n)}
                    expected={(int(4+54*x),int(4+54*y)) for x,y in coords}
                    actual={(x,y) for x in range(64) for y in range(64)
                            if oracle.inside(domain,spacing,x,y)}
                    self.assertEqual(actual,expected)
                    self.assertEqual(oracle.count(domain,spacing),len(expected))
            self.assertEqual(oracle.count(3,0),0)
            self.assertFalse(oracle.inside(0,3,4,4))
