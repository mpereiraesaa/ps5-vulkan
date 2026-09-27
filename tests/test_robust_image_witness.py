# SPDX-License-Identifier: GPL-3.0-or-later
"""Host numerical instruments and real compiler; no GPU robustness claim."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from tools.robust_image_witness import cases, coordinates, expected_read, image_data, linear_index, shader, store_value

ROOT = Path(__file__).resolve().parents[1]


class ImageRobustnessOracle(unittest.TestCase):
    def test_boundaries_and_no_write_races(self):
        for case in cases():
            coords = coordinates(case)
            self.assertEqual(len(coords), len(set(coords)))
            self.assertLessEqual(len(coords), 32)
            self.assertTrue(any(linear_index(case, c) is None for c in coords))
            self.assertTrue(any(linear_index(case, c) is not None for c in coords))
            for axis, bound in enumerate(case.extent):
                values = {c[axis] for c in coords}
                self.assertTrue({-2, -1, 0, bound-1, bound, bound+1, -(1 << 31), (1 << 31)-1} <= values)

    def test_independent_dense_image_and_write_oracle(self):
        for case in cases():
            raw = image_data(case)
            after = image_data(case, True) if case.write else raw
            expected = bytearray(raw)
            for invocation, coord in enumerate(coordinates(case)):
                valid = all(0 <= x < n for x, n in zip(coord, case.extent))
                accepted = expected_read(case, coord)
                if not valid:
                    self.assertEqual(((0,0,0,1),) if case.dimension == 'Buffer' else
                                     ((0,0,0,0),(0,0,0,1)), accepted)
                    self.assertNotIn((1,0,0,0), accepted)
                    self.assertNotIn((0,1,0,1), accepted)
                    self.assertNotIn((0,0,1,0), accepted)
                    self.assertNotIn((0,0,0,2), accepted)
                    continue
                # Row-major indexing computed independently of linear_index.
                index = coord[-1]
                for axis in range(len(coord)-2, -1, -1):
                    index = index * case.extent[axis] + coord[axis]
                if case.components == 1:
                    value, = struct.unpack_from('<I',raw,index*4)
                    self.assertEqual(((value,0,0,1),), accepted)
                    if case.write:
                        struct.pack_into('<I',expected,index*4,store_value(invocation))
                else:
                    self.assertEqual((tuple(raw[index*4:index*4+4]),), accepted)
            self.assertEqual(bytes(expected),after)

    def test_typed_buffer_and_image_requirements_are_distinct(self):
        self.assertEqual(8, sum(c.requirement == 'robustImageAccess' for c in cases()))
        self.assertEqual(3, sum(c.requirement == 'robustBufferAccess2' for c in cases()))
        self.assertEqual(0x54600007, store_value(0))
        self.assertEqual(0x54600108, store_value(1))
        for case in cases():
            self.assertNotIn('textureLod', shader(case))
            self.assertNotIn('coordinates[index].w', shader(case))


class ImageRobustnessCompiler(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        archive=ROOT/'build/libpsbc.host.a'
        glslang=ROOT/'build/runtime-graphics/toolchain/usr/bin/glslangValidator'
        cls.glslang=str(glslang) if glslang.is_file() else shutil.which('glslangValidator')
        if not archive.is_file() or not cls.glslang:
            raise unittest.SkipTest('requires pinned host compiler and GLSLang')
        directory=tempfile.TemporaryDirectory(); cls.addClassCleanup(directory.cleanup)
        cls.tmp=Path(directory.name); cls.exe=cls.tmp/'inspect'
        includes=['src','include','third_party/vulkan-headers/include','third_party/psbc-reference','third_party/opengnm/include']
        sources=['src/ps5vk_compiler.c','src/ps5_compiler_shims.c','tests/robust_image_compiler.c']
        subprocess.run(['cc','-std=c11','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            *['-I'+str(ROOT/p) for p in includes],*[str(ROOT/p) for p in sources],str(archive),
            '-lstdc++','-lm','-lpthread','-o',str(cls.exe)],check=True,capture_output=True,text=True)

    def test_dynamic_coordinates_all_resource_routes(self):
        for case in cases():
            with self.subTest(case=case.name):
                source=self.tmp/(case.name+'.comp'); binary=source.with_suffix('.spv')
                source.write_text(shader(case))
                subprocess.run([self.glslang,'-V','--target-env','vulkan1.1',str(source),'-o',str(binary)],
                               check=True,capture_output=True,text=True)
                result=subprocess.run([str(self.exe),case.role,str(binary)],check=True,
                    capture_output=True,text=True)
                self.assertIn('compiled descriptors=3 gfx=1013 wave=32',result.stdout)
                # Remove only resource access while preserving dynamic SSBO input
                # and output. Static-use metadata must now omit binding zero.
                text=shader(case)
                start=text.index('    imageStore' if case.write else '    output_data.results[index]=')
                text=text[:start]+'    output_data.results[index]=uvec4(input_data.coordinates[index]);\n}\n'
                source.write_text(text)
                subprocess.run([self.glslang,'-V','--target-env','vulkan1.1',str(source),'-o',str(binary)],
                               check=True,capture_output=True,text=True)
                result=subprocess.run([str(self.exe),case.role,str(binary),'without-resource'],
                                      check=True,capture_output=True,text=True)
                self.assertIn('compiled descriptors=2 gfx=1013 wave=32',result.stdout)
