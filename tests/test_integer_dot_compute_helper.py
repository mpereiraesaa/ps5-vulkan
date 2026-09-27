# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate the numerical readback instrument; this does not execute a GPU."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from tools.integer_dot_spirv import cases
from tools.integer_dot_vectors import buffers

ROOT = Path(__file__).resolve().parents[1]


class IntegerDotReadback(unittest.TestCase):
    def test_full_precision_guards_and_input_mutations(self):
        source = r'''
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "examples/dxvk_render_witness/integer_dot_compute.h"
int main(void) {
 (void)integer_dot_compute_witness;
 for(unsigned c=0;c<42;c++) {
  struct integer_dot_data data={0}; struct integer_dot_result observed;
  assert(fread(data.sizes,4,3,stdin)==3);
  unsigned char *mapped[4],*original[4];
  for(unsigned b=0;b<4;b++) {
   unsigned n=b==3?512:data.sizes[b];
   original[b]=malloc(n); mapped[b]=malloc(n+512); assert(original[b] && mapped[b]);
   assert(fread(original[b],1,n,stdin)==n);
   memset(mapped[b],0xcd,n+512); memcpy(mapped[b]+256,original[b],n);
   if(b<3)data.inputs[b]=original[b]; else data.expected=original[b];
  }
  integer_dot_check(&data,mapped,&observed);
  assert(observed.outputs==128 && !observed.mismatches && !observed.guards && !observed.input_changes);
  printf("%08x\n",observed.digest);
  /* All output bits, including the least significant ones float32 would lose. */
  for(unsigned pos=0;pos<512;pos++) for(unsigned bit=0;bit<8;bit++) {
   mapped[3][256+pos]^=1u<<bit; integer_dot_check(&data,mapped,&observed);
   assert(observed.mismatches==1 && !observed.guards && !observed.input_changes);
   mapped[3][256+pos]^=1u<<bit;
  }
  for(unsigned b=0;b<4;b++) {
   unsigned n=b==3?512:data.sizes[b];
   unsigned offsets[]={0,255,256+n,511+n};
   for(unsigned j=0;j<4;j++) {
    mapped[b][offsets[j]]^=1; integer_dot_check(&data,mapped,&observed);
    assert(observed.guards==1 && !observed.mismatches && !observed.input_changes);
    mapped[b][offsets[j]]^=1;
   }
   if(b<3) for(unsigned pos=0;pos<n;pos++) {
    mapped[b][256+pos]^=1; integer_dot_check(&data,mapped,&observed);
    assert(observed.input_changes==1 && !observed.guards && !observed.mismatches);
    mapped[b][256+pos]^=1;
   }
  }
  memset(mapped[3]+256,0xcd,512); integer_dot_check(&data,mapped,&observed);
  assert(observed.mismatches && !observed.guards && !observed.input_changes);
  for(unsigned b=0;b<4;b++) {free(original[b]);free(mapped[b]);}
 }
 return 0;
}
'''
        payload = bytearray()
        digests = []
        for case in cases():
            values = buffers(case)
            payload.extend(struct.pack('<3I', *map(len, values[:3])))
            payload.extend(b''.join(values))
            digest = 2166136261
            for value in struct.unpack('<128I', values[3]):
                digest = ((digest ^ value) * 16777619) & 0xffffffff
            digests.append(f'{digest:08x}')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); src=root/'instrument.c'; exe=root/'instrument'
            src.write_text(source)
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-I'+str(ROOT), '-I'+str(ROOT/'third_party/vulkan-headers/include'),
                            str(src), '-o', str(exe)], check=True, capture_output=True)
            run = subprocess.run([str(exe)], input=payload, capture_output=True, check=True)
            self.assertEqual(b'', run.stderr)
            self.assertEqual(digests, run.stdout.decode().splitlines())

    def test_real_compiler_and_synthetic_queue(self):
        from tools.integer_dot_spirv import binary
        archive = ROOT/'build/libpsbc.host.a'
        if not archive.is_file():
            self.skipTest('requires pinned host PSBC archive')
        # Resolve the maintained driver source list without building shared
        # targets. All objects and fixtures below live in a temporary directory.
        sources = subprocess.check_output([
            'make', '-s', '--no-print-directory',
            '--eval=print-dot-driver-sources: ; @echo $(VK_DEVICE_SOURCES)',
            'print-dot-driver-sources'], cwd=ROOT, text=True).split()
        self.assertIn('src/vk_device.c', sources)
        payload=bytearray()
        for case in cases():
            shader=binary(case)
            payload.extend(struct.pack('<5I', {'u':0,'s':1,'su':2}[case.mode], case.components,
                                       case.saturating, case.packed_types is not None,len(shader)))
            payload.extend(shader)
            payload.extend(b''.join(buffers(case)))
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'execution'
            compiled=subprocess.run(['cc','-std=c11','-g','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                            '-Ithird_party/vulkan-headers/include','-Isrc','-Iinclude',
                            '-Ithird_party/psbc-reference','-Ithird_party/opengnm/include',
                            *sources,'src/platform_host.c','src/ps5vk_compiler.c','src/ps5_compiler_shims.c',
                            'tests/integer_dot_execution.c',str(archive),'-lstdc++','-lm','-lpthread',
                            '-o',str(exe)],cwd=ROOT,capture_output=True)
            self.assertEqual(0,compiled.returncode,compiled.stderr.decode())
            run=subprocess.run([str(exe)],input=payload,capture_output=True)
            self.assertEqual(0,run.returncode,run.stderr.decode())
            self.assertIn(b'42 integer-dot variants',run.stdout)
            self.assertNotIn(b'ERROR: AddressSanitizer',run.stderr)
            timed=subprocess.run([str(exe),'--timeout'],input=payload,capture_output=True)
            self.assertEqual(0,timed.returncode,timed.stderr.decode())
            self.assertIn(b'bounded timeout retained live resources',timed.stdout)
