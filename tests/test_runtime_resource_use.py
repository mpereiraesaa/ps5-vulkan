import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class RuntimeResourceUseTests(unittest.TestCase):
    def test_three_stage_union_and_unknown_dominance(self):
        source = r'''
#include "native/runtime_resource_use.h"
#include <assert.h>
int main(void) {
    struct ps5vk_runtime_draw_abi raster={.enabled=1}, hull={.enabled=1};
    assert(!ps5vk_runtime_resource_bindings(&raster,&hull,2));
    hull.vertex_descriptor_valid[2]=1;
    hull.vertex_used_bindings[2]=1ull<<63;
    assert(ps5vk_runtime_resource_bindings(&raster,&hull,2)==(1ull<<63));
    raster.vertex_descriptor_valid[2]=raster.fragment_descriptor_valid[2]=1;
    raster.vertex_used_bindings[2]=2;raster.fragment_used_bindings[2]=8;
    assert(ps5vk_runtime_resource_bindings(&raster,&hull,2)==((1ull<<63)|10));
    /* Every stage independently turns an otherwise precise union unknown. */
    for(unsigned stage=0;stage<3;++stage) {
        uint64_t *mask=stage==0?&raster.vertex_used_bindings[2]:
            stage==1?&raster.fragment_used_bindings[2]:&hull.vertex_used_bindings[2];
        uint64_t saved=*mask;*mask=0;
        assert(ps5vk_runtime_resource_bindings(&raster,&hull,2)==UINT64_MAX);
        *mask=saved;
    }
    hull.enabled=0;
    assert(ps5vk_runtime_resource_bindings(&raster,&hull,2)==10);
    raster.vertex_descriptor_valid[2]=raster.fragment_descriptor_valid[2]=0;
    assert(!ps5vk_runtime_resource_bindings(&raster,&hull,2));
    assert(!ps5vk_runtime_resource_bindings(&raster,&hull,4));
    assert(!ps5vk_runtime_resource_bindings(0,0,0));
    uint32_t bytes=99;
    hull.enabled=1;hull.push_constant_size=16;
    assert(!ps5vk_runtime_push_bytes(&raster,&hull,32,&bytes) && bytes==16);
    raster.push_constant_size=48;
    assert(!ps5vk_runtime_push_bytes(&raster,&hull,64,&bytes) && bytes==48);
    hull.push_constant_size=96;
    assert(!ps5vk_runtime_push_bytes(&raster,&hull,128,&bytes) && bytes==96);
    bytes=99;
    assert(ps5vk_runtime_push_bytes(&raster,&hull,64,&bytes) && bytes==99);
    assert(ps5vk_runtime_push_bytes(&raster,&hull,258,&bytes));
    hull.enabled=0;assert(ps5vk_runtime_push_bytes(&raster,&hull,128,&bytes));
    hull.enabled=1;hull.push_constant_size=257;
    assert(ps5vk_runtime_push_bytes(&raster,&hull,256,&bytes));
    hull.push_constant_size=raster.push_constant_size=0;
    assert(!ps5vk_runtime_push_bytes(&raster,&hull,32,&bytes) && !bytes);
    uint32_t visibility[64]={0};visibility[0]=1;
    visibility[4]=visibility[5]=visibility[6]=2;
    assert(ps5vk_runtime_push_visibility(1,visibility,8,1));
    assert(ps5vk_runtime_push_visibility(0x70,visibility,8,2));
    assert(!ps5vk_runtime_push_visibility(1,visibility,8,2));
    assert(!ps5vk_runtime_push_visibility(0x70,visibility,6,2));
    visibility[63]=1;
    assert(ps5vk_runtime_push_visibility(UINT64_C(1)<<63,visibility,64,1));
    assert(!ps5vk_runtime_push_visibility(UINT64_C(1)<<63,visibility,63,1));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = pathlib.Path(directory) / "resource-use"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
                 "-x", "c", "-", "-o", str(executable)],
                input=source, text=True, capture_output=True, check=True)
            subprocess.run([str(executable)], check=True, capture_output=True)
