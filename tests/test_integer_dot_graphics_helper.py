# SPDX-License-Identifier: GPL-3.0-or-later
"""Graphics readback instrumentation, with no GPU execution claim."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from tools.integer_dot_spirv import cases
from tools.integer_dot_graphics_witness import image_fixture
from tools.integer_dot_vectors import buffers

ROOT=Path(__file__).resolve().parents[1]


class IntegerDotGraphicsHelper(unittest.TestCase):
    def test_rgba_readback_full_bits_guards_and_inputs(self):
        source=r'''
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "examples/dxvk_render_witness/integer_dot_graphics.h"
int main(void) {
 (void)integer_dot_graphics_witness;
 unsigned char records[32768],expected[512],input[33280],image[1024];
 struct integer_dot_graphics_case data={.records=records,.expected_rgba=expected};
 for(unsigned c=0;c<42;++c) {
  assert(fread(records,1,sizeof(records),stdin)==sizeof(records));
  assert(fread(expected,1,sizeof(expected),stdin)==sizeof(expected));
  memset(input,0xcd,sizeof(input));memset(image,0xcd,sizeof(image));
  memcpy(input+256,records,sizeof(records));memcpy(image+256,expected,sizeof(expected));
  struct integer_dot_graphics_result out;
  integer_dot_graphics_check(&data,input,image,&out);
  assert(out.pixels==128 && !out.mismatches && !out.guards && !out.input_changes);
  printf("%08x\n",out.digest);
  const unsigned pixels[]={0,63,64,127};
  for(unsigned p=0;p<4;++p)for(unsigned b=0;b<4;++b)for(unsigned bit=0;bit<8;++bit) {
   unsigned at=256+4*pixels[p]+b;image[at]^=1u<<bit;
   integer_dot_graphics_check(&data,input,image,&out);
   assert(out.mismatches==1 && !out.guards && !out.input_changes);image[at]^=1u<<bit;
  }
  for(unsigned record=0;record<128;++record)for(unsigned byte=0;byte<2;++byte) {
   unsigned at=256+record*256+(byte?255:0);input[at]^=1;
   integer_dot_graphics_check(&data,input,image,&out);
   assert(out.input_changes==1 && !out.guards && !out.mismatches);input[at]^=1;
  }
  for(unsigned which=0;which<2;++which) {
   unsigned char *memory=which?image:input;unsigned size=which?512:32768;
   unsigned positions[]={0,255,256+size,511+size};
   for(unsigned n=0;n<4;++n) {
    memory[positions[n]]^=1;integer_dot_graphics_check(&data,input,image,&out);
    assert(out.guards==1 && !out.mismatches && !out.input_changes);memory[positions[n]]^=1;
   }
  }
  memset(image+256,0xcd,512);integer_dot_graphics_check(&data,input,image,&out);
  assert(out.mismatches && !out.guards && !out.input_changes);
 }
 return 0;
}
'''
        payload=bytearray();digests=[]
        for case in cases():
            payload.extend(b''.join(image_fixture(case,bgra=False)))
            digest=2166136261
            for word in struct.unpack('<128I',buffers(case)[3]):
                digest=((digest^word)*16777619)&0xffffffff
            digests.append(f'{digest:08x}')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);src=root/'check.c';exe=root/'check';src.write_text(source)
            compile=subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                '-I'+str(ROOT),'-I'+str(ROOT/'third_party/vulkan-headers/include'),
                str(src),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(0,compile.returncode,compile.stderr)
            result=subprocess.run([str(exe)],input=payload,capture_output=True)
            self.assertEqual(0,result.returncode,result.stderr.decode())
            self.assertEqual(digests,result.stdout.decode().splitlines())
            self.assertEqual(b'',result.stderr)

    def test_real_compiler_queue_and_native_readback_plan(self):
        import os
        from test_integer_dot_graphics_numeric import IntegerDotNumericCompiler
        from tools.integer_dot_graphics_witness import STAGES,GRAPHS
        # Reuse the permanent fixture compilation setup. Its outputs and the
        # executable below live only in temporary directories, with no SDK or
        # shared build writes; no SERIAL test-runner slot is needed.
        IntegerDotNumericCompiler.setUpClass()
        self.addCleanup(IntegerDotNumericCompiler.doClassCleanups)
        fixtures=IntegerDotNumericCompiler
        payload=bytearray(struct.pack('<I',len(cases())*len(GRAPHS)))
        for case in cases():
            for target,graph in GRAPHS.items():
                payload.extend(struct.pack('<4I',{'u':0,'s':1,'su':2}[case.mode],case.components,
                                           case.saturating,case.packed_types is not None))
                for stage in STAGES:
                    path=(fixtures.variants[stage,case] if target=='all' or stage==target else fixtures.controls[stage])
                    data=path.read_bytes() if stage in graph else b''
                    payload.extend(struct.pack('<I',len(data)));payload.extend(data)
                payload.extend(b''.join(image_fixture(case,bgra=False)))
        sources=subprocess.check_output(['make','-s','--no-print-directory',
            '--eval=print-dot-driver-sources: ; @echo $(VK_DEVICE_SOURCES)',
            'print-dot-driver-sources'],cwd=ROOT,text=True).split()
        gears=Path(os.environ.get('LAB_SIBLINGS',ROOT.parent))/'ps5-agc-gears'
        includes=[ROOT/'src',ROOT/'native',ROOT/'include',gears/'src',gears/'include',
                  ROOT/'third_party/vulkan-headers/include',ROOT/'third_party/psbc-reference']
        extra=['src/platform_host.c','native/image_ps5.c','src/depth_layout.c','src/image_layout_state.c',
               'native/runtime_shader.c','native/runtime_graphics_compiler.c','src/spirv_graphics_interface.c',
               'src/ps5_compiler_shims.c','src/texture_dma.c','src/graphics_sync.c','tests/integer_dot_graphics_execution.c']
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'execute'
            compiled=subprocess.run(['cc','-std=c11','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all',*['-I'+str(p) for p in includes],
                *sources,*extra,'build/libpsbc.host.a','-lstdc++','-lm','-lpthread','-o',str(exe)],
                cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(0,compiled.returncode,compiled.stderr)
            result=subprocess.run([str(exe)],input=payload,capture_output=True)
            self.assertEqual(0,result.returncode,result.stderr.decode())
            self.assertIn(b'276 graphics executions',result.stdout)
            timed=subprocess.run([str(exe),'--timeout'],input=payload,capture_output=True)
            self.assertEqual(0,timed.returncode,timed.stderr.decode())
            self.assertIn(b'bounded graphics timeout retained resources',timed.stdout)
