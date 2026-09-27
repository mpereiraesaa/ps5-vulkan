# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate the SDK readback instrument, not GPU image robustness."""
from pathlib import Path
import subprocess
import shutil
import re
import tempfile
import unittest

from tools.robust_image_witness import cases, shader
from tools.build_robust_image_witness import fixture_header, diagnostic_environment
from tools.build_upstream_cts import tessellation_build_profile
from tools.verify_robust_image_witness import resource_contract

ROOT = Path(__file__).resolve().parents[1]


class RobustImageExecution(unittest.TestCase):
    def test_build_profile_is_isolated(self):
        environment={key:'1' for key in tessellation_build_profile({})['switches']}
        environment['PATH']='/bin'
        self.assertEqual({'PATH':'/bin','PS5_PAYLOAD_SDK':'/sdk','PS5VK_USE_SDK':'1',
            'PS5VK_IMAGE_ROBUSTNESS_DIAGNOSTIC':'1'},diagnostic_environment(environment,'/sdk'))

    def test_readback_positive_controls_and_corruption(self):
        source=r'''
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "examples/robust_image_witness/compute.h"
#include "fixture.h"
int main(void) {
 (void)robust_image_compute;(void)robust_image_spirv;
 const struct robust_image_data *d=&robust_fixture;
 unsigned char *mapped[4];unsigned sizes[]={16*d->count,16*d->count,d->image_bytes,d->image_bytes};
 unsigned texel=d->type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || d->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
 unsigned sampled=d->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
 for(unsigned b=0;b<4;++b){mapped[b]=malloc(sizes[b]+512);assert(mapped[b]);memset(mapped[b],0xcd,sizes[b]+512);}
 memcpy(mapped[0]+256,d->coordinates,sizes[0]);memcpy(mapped[1]+256,d->expected,sizes[1]);
 memcpy(mapped[2]+256,texel?d->image_after:d->image,sizes[2]);memcpy(mapped[3]+256,d->image_after,sizes[3]);
 struct robust_image_result out={0};robust_image_check(d,mapped,&out);
 assert(out.resource_digest==EXPECTED_RESOURCE_DIGEST && out.resource_bytes==EXPECTED_RESOURCE_BYTES);
 assert(!memcmp(out.values,d->expected,16*d->count));
 assert(out.outputs==d->count && !out.mismatches && !out.image_changes && !out.input_changes && !out.guards);
 for(unsigned i=0;i<d->count;++i)for(unsigned component=0;component<4;++component)for(unsigned bit=0;bit<32;++bit){
  uint32_t *value=(uint32_t*)(mapped[1]+256)+4*i+component;
  *value^=1u<<bit;robust_image_check(d,mapped,&out);
  assert(out.values[i][component]==*value);
  assert(out.mismatches==(component==3 && bit==0 && d->alpha_either[i]?0u:1u));
  assert(!out.image_changes && !out.input_changes && !out.guards);*value^=1u<<bit;
 }
 for(unsigned b=0;b<((texel||sampled)?3u:4u);++b){
  for(unsigned i=0;i<256;++i)for(unsigned side=0;side<2;++side){
   unsigned pos=side?256+sizes[b]+i:i;mapped[b][pos]^=1;robust_image_check(d,mapped,&out);
   assert(out.guards==1 && !out.mismatches && !out.input_changes && !out.image_changes);mapped[b][pos]^=1;
  }
 }
 for(unsigned i=0;i<sizes[0];++i){mapped[0][256+i]^=1;robust_image_check(d,mapped,&out);
  assert(out.input_changes==1 && !out.mismatches && !out.guards && !out.image_changes);mapped[0][256+i]^=1;}
 if(!texel)for(unsigned i=0;i<sizes[2];++i){mapped[2][256+i]^=1;robust_image_check(d,mapped,&out);
  assert(out.input_changes==1 && !out.mismatches && !out.guards && !out.image_changes);mapped[2][256+i]^=1;}
 if(!sampled)for(unsigned i=0;i<d->image_bytes;++i){mapped[texel?2:3][256+i]^=1;robust_image_check(d,mapped,&out);
  assert(out.image_changes==1 && !out.mismatches && !out.guards && !out.input_changes);mapped[texel?2:3][256+i]^=1;}
 memset(mapped[1]+256,0xcd,sizes[1]);robust_image_check(d,mapped,&out);assert(out.mismatches==4*d->count);
 for(unsigned b=0;b<4;++b)free(mapped[b]);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp);c=directory/'check.c';exe=directory/'check';c.write_text(source)
            for case in cases():
                with self.subTest(case=case.name):
                    (directory/'fixture.h').write_text(fixture_header(case,b'\x03\x02\x23\x07')+
                        '#define EXPECTED_RESOURCE_DIGEST 0x'+resource_contract(case)['digest']+'u\n'+
                        '#define EXPECTED_RESOURCE_BYTES '+str(resource_contract(case)['bytes'])+'u\n')
                    result=subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                        '-I'+str(ROOT),'-I'+str(ROOT/'third_party/vulkan-headers/include'),
                        str(c),'-o',str(exe)],capture_output=True,text=True)
                    self.assertEqual(0,result.returncode,result.stderr)
                    result=subprocess.run([str(exe)],capture_output=True,text=True)
                    self.assertEqual(0,result.returncode,result.stderr)

    def test_real_frontend_compiler_and_synthetic_execution(self):
        archive=ROOT/'build/libpsbc.host.a'
        local=ROOT/'build/runtime-graphics/toolchain/usr/bin/glslangValidator'
        glslang=str(local) if local.is_file() else shutil.which('glslangValidator')
        if not archive.is_file() or not glslang:
            self.skipTest('requires local compiler and GLSLang')
        sources=subprocess.check_output(['make','-s','--no-print-directory',
            '--eval=print-image-driver-sources: ; @echo $(VK_DEVICE_SOURCES)',
            'print-image-driver-sources'],cwd=ROOT,text=True).split()
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp);headers=[];exe=directory/'execute'
            for i,case in enumerate(cases()):
                source=directory/'shader.comp';binary=directory/'shader.spv';source.write_text(shader(case))
                subprocess.run([glslang,'-V','--target-env','vulkan1.1',str(source),'-o',str(binary)],check=True,capture_output=True)
                header=fixture_header(case,binary.read_bytes())
                header=re.sub(r'^#define RI_CASE_NAME.*$', '', header, flags=re.MULTILINE)
                for name in ['ri_coordinates','ri_image','ri_after','ri_expected','ri_alpha_either','robust_fixture','robust_image_spirv']:
                    header=re.sub(r'\b'+name+r'\b',name+'_'+str(i),header)
                headers.append(header)
            headers.append('static const struct robust_image_data *fixtures[]={'+','.join('&robust_fixture_'+str(i) for i in range(11))+'};')
            headers.append('static const uint32_t *shaders[]={'+','.join('robust_image_spirv_'+str(i) for i in range(11))+'};')
            headers.append('static const size_t shader_bytes[]={'+','.join('sizeof(robust_image_spirv_'+str(i)+')' for i in range(11))+'};')
            (directory/'fixtures.h').write_text('\n'.join(headers))
            command=['cc','-std=c11','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-fno-sanitize-recover=all','-I'+str(directory),'-Ithird_party/vulkan-headers/include',
                '-Isrc','-Iinclude','-Ithird_party/psbc-reference','-Ithird_party/opengnm/include',
                *sources,'src/platform_host.c','src/ps5vk_compiler.c','src/ps5_compiler_shims.c',
                'src/descriptor_encode.c','src/texture_descriptor.c','src/depth_layout.c','native/image_ps5.c',
                'tests/robust_image_execution.c',str(archive),'-lstdc++','-lm','-lpthread','-o',str(exe)]
            result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(0,result.returncode,result.stderr)
            for args in ([],['--timeout']):
                result=subprocess.run([str(exe),*args],capture_output=True,text=True)
                self.assertEqual(0,result.returncode,result.stderr)
                self.assertIn('bounded timeout' if args else '11 variants:',result.stdout)
