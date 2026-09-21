"""Owned SPIR-V stage-pair execution-mode contract, not GPU evidence."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
PROBE=r'''
#include "spirv_graphics_interface.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
static struct ps5vk_graphics_module_key read_module(const char *path) {
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
 uint32_t *p=malloc(n);assert(p && fread(p,1,n,f)==(size_t)n);fclose(f);
 return (struct ps5vk_graphics_module_key){.words=p,.word_count=n/4,.entry="main"};
}
int main(int argc,char **argv) {
 assert(argc==5);
 struct ps5vk_graphics_key k={.vertex=read_module(argv[1]),
  .tess_control=read_module(argv[2]),.tess_eval=read_module(argv[3]),
  .fragment=read_module(argv[4]),.patch_control_points=3,
  .topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
  /* A colour subpass: the probe's fragment stage writes a colour. An unset
     format means a depth-only pass, whose fragment stage exports nothing. */
  .color_format=VK_FORMAT_B8G8R8A8_UNORM,.color_write_mask=15};
 int valid=ps5vk_spirv_graphics_interface(&k);
 assert(ps5vk_spirv_tess_pair_output_points(&k)==(valid?3u:0u));
 printf("%d\n",valid);
 free((void*)k.vertex.words);free((void*)k.tess_control.words);
 free((void*)k.tess_eval.words);free((void*)k.fragment.words);
 return 0;
}
'''

def unpack(path):
    blob=path.read_bytes();words=list(struct.unpack('<'+'I'*(len(blob)//4),blob))
    ops=[];i=5
    while i<len(words):
        n=words[i]>>16
        if not n:raise ValueError('invalid instruction')
        ops.append(words[i:i+n]);i+=n
    return words[:5],ops

def pack(path,module):
    header,ops=module;words=header+[x for op in ops for x in op]
    path.write_bytes(struct.pack('<'+'I'*len(words),*words))

def relocate(source,target,modes,retain=False):
    entry=next(op[2] for op in target[1] if op[0]&65535==15)
    moved=[op[:] for op in source[1] if op[0]&65535==16 and op[2] in modes]
    for op in moved:op[1]=entry
    index=next((i for i,op in enumerate(target[1]) if op[0]&65535==16),
               next(i for i,op in enumerate(target[1]) if op[0]&65535==15)+1)
    target[1][index:index]=moved
    if not retain:source[1][:]=[op for op in source[1] if not(op[0]&65535==16 and op[2] in modes)]

class ModePlacementTests(unittest.TestCase):
    def test_pair_union_and_conflicts(self):
        glslang=shutil.which('glslangValidator')
        if not glslang:self.skipTest('glslang required')
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)
            def run(args,**kwargs):
                return subprocess.check_output(args,cwd=ROOT,text=True,stderr=subprocess.STDOUT,**kwargs)
            for ext,source in [('vert','runtime_tess_coord.vert'),('tesc','runtime_tess_coord.tesc'),
                               ('tese','runtime_tess_output_envelope.tese'),('frag','runtime_tess_output_envelope.frag')]:
                run([glslang,'-V','experiments/graphics/'+source,'-o',str(p/ext)])
            run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Ithird_party/vulkan-headers/include',
                 '-Isrc','src/spirv_graphics_interface.c','src/texture_format.c','-x','c','-',
                 '-o',str(p/'probe')],input=PROBE)
            cases=('normal','moved','split','identical','conflict','missing','output-in-eval')
            for case in cases:
                with self.subTest(case=case):
                    c,e=unpack(p/'tesc'),unpack(p/'tese')
                    if case in ('moved','identical','conflict'):
                        relocate(e,c,{1,4,22},retain=case!='moved')
                    if case=='split':relocate(e,c,{1,22})
                    if case=='output-in-eval':relocate(c,e,{26})
                    if case=='conflict':
                        next(op for op in c[1] if op[0]&65535==16 and op[2]==22)[2]=24
                    if case=='missing':e[1][:]=[op for op in e[1] if not(op[0]&65535==16 and op[2]==22)]
                    pack(p/'c.spv',c);pack(p/'e.spv',e)
                    got=run([str(p/'probe'),str(p/'vert'),str(p/'c.spv'),str(p/'e.spv'),str(p/'frag')]).strip()
                    self.assertEqual(got,'0' if case in ('conflict','missing') else '1')
