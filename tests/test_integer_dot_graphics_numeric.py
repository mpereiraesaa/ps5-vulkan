# SPDX-License-Identifier: GPL-3.0-or-later
"""Permanent bit-exact graphics fixtures; host compiler tests are not GPU evidence."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from tools.integer_dot_spirv import Case, cases, integerize_dot_add
from tools.integer_dot_vectors import reference, samples
from tools.integer_dot_graphics_witness import GRAPHS, STAGES, image_fixture, template

ROOT = Path(__file__).resolve().parents[1]


def instructions(words):
    offset = 5
    while offset < len(words):
        count = words[offset] >> 16
        assert count and offset+count <= len(words)
        yield offset, words[offset] & 65535, words[offset+1:offset+count]
        offset += count


class IntegerDotImageData(unittest.TestCase):
    def test_exact_records_padding_and_bgra_order(self):
        for case in cases():
            records, image = image_fixture(case)
            self.assertEqual((32768, 512), (len(records), len(image)))
            for i, row in enumerate(samples(case)):
                raw = records[256*i:256*(i+1)]
                used = set()
                for offset, values in ((0, row.lhs), (16, row.rhs)):
                    expected = bytes(values) if case.packed_types is not None else struct.pack(f'<{case.components}I', *values)
                    self.assertEqual(expected, raw[offset:offset+len(expected)])
                    used.update(range(offset, offset+len(expected)))
                self.assertEqual(row.accumulator, struct.unpack('<I', raw[32:36])[0])
                used.update(range(32,36))
                self.assertTrue(all(byte==0xa5 for pos,byte in enumerate(raw) if pos not in used))
                b,g,r,a = image[4*i:4*i+4]
                self.assertEqual(reference(case,row), r | g<<8 | b<<16 | a<<24)
        self.assertEqual(b'\0\0\1\0\0\0\2\0',image_fixture(Case('u',2))[1][:8])

    def test_byte_normalization_and_instance_coverage(self):
        for byte in range(256):
            encoded = struct.unpack('<f',struct.pack('<f',byte/255.0))[0]
            self.assertEqual(byte,round(encoded*255))
        quads = [((i%16,i//16),(i%16+1,i//16+1)) for i in range(128)]
        self.assertEqual(128,len(set(quads)))
        self.assertEqual(((15,7),(16,8)),quads[-1])
        # Both position divisors are powers of two, so quad boundaries are
        # exactly representable in float32 before viewport transformation.
        for lo,hi in quads:
            for x,y in (lo,hi):
                self.assertEqual(x/8-1,struct.unpack('<f',struct.pack('<f',x/8-1))[0])
                self.assertEqual(y/4-1,struct.unpack('<f',struct.pack('<f',y/4-1))[0])

    def test_template_rejects_unknown_shapes(self):
        for stage,n in (('compute',4),('vert',1),('frag',5)):
            with self.assertRaises(ValueError): template(stage,n)
        with self.assertRaises(ValueError): integerize_dot_add([],Case('u',2),preserve_result_bits=1)


class IntegerDotNumericCompiler(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        archive=ROOT/'build/libpsbc.host.a'
        cls.glslang=shutil.which('glslangValidator')
        fallback=ROOT/'build/runtime-graphics/toolchain/usr/bin/glslangValidator'
        if not cls.glslang and fallback.is_file(): cls.glslang=str(fallback)
        gears=Path(os.environ.get('LAB_SIBLINGS',ROOT.parent))/'ps5-agc-gears'
        if not archive.is_file() or not cls.glslang or not (gears/'include').is_dir():
            raise unittest.SkipTest('requires pinned host PSBC, glslang and graphics headers')
        directory=tempfile.TemporaryDirectory();cls.addClassCleanup(directory.cleanup)
        cls.tmp=Path(directory.name);cls.exe=cls.tmp/'inspect'
        includes=[ROOT/'src',ROOT/'native',gears/'src',gears/'include',
                  ROOT/'third_party/vulkan-headers/include',ROOT/'third_party/psbc-reference']
        sources=['tests/integer_dot_graphics.c','native/runtime_shader.c','native/runtime_graphics_compiler.c',
                 'src/color_attachment_contract.c','src/spirv_graphics_interface.c','src/texture_format.c','src/ps5_compiler_shims.c']
        compiled=subprocess.run(['cc','-std=c11','-g','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',*['-I'+str(p) for p in includes],
            *sources,str(archive),'-lstdc++','-lm','-lpthread','-o',str(cls.exe)],cwd=ROOT,capture_output=True,text=True)
        if compiled.returncode: raise RuntimeError(compiled.stderr)
        cls.controls={};cls.templates={};cls.variants={}
        for stage in STAGES:
            for n in (0,2,3,4):
                path=cls.tmp/f'base{n}.{stage}';path.write_text(template(stage,n));sp=Path(str(path)+'.spv')
                result=subprocess.run([cls.glslang,'-V',str(path),'-o',str(sp)],capture_output=True,text=True)
                if result.returncode: raise RuntimeError(result.stdout+result.stderr)
                if not n: cls.controls[stage]=sp
                else: cls.templates[stage,n]=list(struct.unpack(f'<{sp.stat().st_size//4}I',sp.read_bytes()))
        for case in cases():
            for stage in STAGES:
                words=integerize_dot_add(cls.templates[stage,case.components],case,preserve_result_bits=True)
                path=cls.tmp/f'{case.name}.{stage}.spv';path.write_bytes(struct.pack(f'<{len(words)}I',*words))
                cls.variants[stage,case]=path

    def inspect(self,target,active,variants):
        modules={s:variants.get(s,self.controls[s]) for s in GRAPHS[target]}
        result=subprocess.run([str(self.exe),*[str(modules.get(s,'-')) for s in STAGES]],capture_output=True,text=True)
        self.assertEqual(0,result.returncode,result.stderr)
        expected=(0,int(bool(active&{'vert','tesc'})),int(bool(active&{'tese','geom'})),int('frag' in active)) if 'tesc' in modules else (int(bool(active&{'vert','geom'})),0,0,int('frag' in active))
        self.assertEqual(expected,tuple(map(int,result.stdout.split())))

    def test_all_stages_and_merged_graphs_with_controls(self):
        for target in GRAPHS: self.inspect(target,set(),{})
        for case in cases():
            for target in GRAPHS:
                with self.subTest(case=case.name,target=target):
                    active=set(STAGES if target=='all' else (target,))
                    self.inspect(target,active,{s:self.variants[s,case] for s in active})

    def test_only_result_conversion_changes_to_bitcast(self):
        for case in cases():
            for stage in STAGES:
                template_words=self.templates[stage,case.components]
                ordinary=integerize_dot_add(template_words,case)
                exact=integerize_dot_add(template_words,case,preserve_result_bits=True)
                differing=[i for i,(a,b) in enumerate(zip(ordinary,exact)) if a!=b]
                self.assertEqual(len(ordinary),len(exact));self.assertEqual(1,len(differing))
                index=differing[0]
                self.assertEqual(112 if case.mode=='u' else 111,ordinary[index]&65535)
                self.assertEqual(124,exact[index]&65535)
                result=exact[index+2]
                consumers=[args for _,op,args in instructions(exact) if op==124 and args[2]==result]
                self.assertEqual(1,len(consumers))
                types={args[0]:args[1:] for _,op,args in instructions(exact) if op==21}
                self.assertEqual([32,0],types[consumers[0][0]])
