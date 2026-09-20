"""Visibility-only discard oracle; native invocation counts are not implied."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class DiscardOracleTests(unittest.TestCase):
    def test_every_patch_and_background(self):
        with tempfile.TemporaryDirectory() as directory:
            lib=Path(directory)/"oracle.so"
            subprocess.run(["cc","-shared","-fPIC","-Wall","-Werror","-Inative",
                "-x","c","-","-o",str(lib)],cwd=ROOT,check=True,text=True,
                input='#include "tess_discard_oracle.h"\n'
                'int pixel(unsigned d,unsigned x,unsigned y) '
                '{return ps5vk_tess_discard_pixel(d,x,y);}\n')
            oracle=ctypes.CDLL(str(lib))
            for domain,relevant in enumerate((3,4,2)):
                expected={0,13,14,15,16}|set(range(1+3*relevant,13))
                observed=set()
                for y in range(64):
                    for x in range(64):
                        got=oracle.pixel(domain,x,y)
                        if got>=0:
                            self.assertEqual((x,y),(6+12*(got%5),6+12*(got//5)))
                            observed.add(got)
                self.assertEqual(observed,expected)
                self.assertIn(0,observed)  # An empty image can never pass.
            self.assertEqual(oracle.pixel(3,6,6),-1)
