"""Pure register-contract tests; no claim of native blend execution."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class BlendRegisterTests(unittest.TestCase):
    def test_equations_factors_constants_and_rejection(self):
        source = r'''
#include "blend_ps5.h"
#include <assert.h>
int main(void) {
    const float c[4]={0.0f,0.25f,0.5f,1.0f};
    struct ps5vk_blend_words w;
    VkPipelineColorBlendAttachmentState a={.blendEnable=VK_TRUE,
        .srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor=VK_BLEND_FACTOR_ONE,
        .srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA,
        .dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE};
    assert(ps5vk_blend_encode(&a,c,VK_FALSE,&w));
    assert(w.control==0x40000104u && w.optimization==0x00770077u);
    assert(w.constants[0]==0 && w.constants[1]==0x3e800000u &&
        w.constants[2]==0x3f000000u && w.constants[3]==0x3f800000u);
    a.alphaBlendOp=VK_BLEND_OP_REVERSE_SUBTRACT;
    assert(ps5vk_blend_encode(&a,c,VK_FALSE,&w));
    assert(w.control==0x61840104u);
    const uint32_t factor_words[]={0,1,2,3,8,9,4,5,6,7,13,14,19,20,10,15,16,17,18};
    for(unsigned i=0;i<19;++i) {
        uint32_t x=~0u;
        assert(ps5vk_blend_factor((VkBlendFactor)i,&x) && x==factor_words[i]);
    }
    a.srcColorBlendFactor=VK_BLEND_FACTOR_SRC1_COLOR;
    a.dstColorBlendFactor=VK_BLEND_FACTOR_ZERO;
    a.srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC1_ALPHA;
    a.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;
    a.colorBlendOp=a.alphaBlendOp=VK_BLEND_OP_ADD;
    assert(!ps5vk_blend_encode(&a,c,VK_FALSE,&w));
    assert(!w.control && !w.optimization);
    assert(ps5vk_blend_encode(&a,c,VK_TRUE,&w));
    assert(w.control==0x6011000fu && w.optimization==0x00770077u);
    a.srcColorBlendFactor=(VkBlendFactor)999;
    assert(!ps5vk_blend_encode(&a,c,VK_TRUE,&w) && !w.control && !w.optimization);
    a.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
    a.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;
    a.srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
    a.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
    const uint32_t fn[]={0,1,4,2,3};
    for(unsigned i=0;i<5;++i) {
        a.colorBlendOp=a.alphaBlendOp=(VkBlendOp)i;
        assert(ps5vk_blend_encode(&a,c,VK_FALSE,&w));
        unsigned factors=i>=3?0x101u:0x104u;
        assert(w.control==((1u<<30)|(fn[i]<<5)|factors));
    }
    a.colorBlendOp=(VkBlendOp)999;
    assert(!ps5vk_blend_encode(&a,c,VK_FALSE,&w) && !w.control);
    a.blendEnable=VK_FALSE;
    assert(ps5vk_blend_encode(&a,0,VK_FALSE,&w) && !w.control);
    for(unsigned i=0;i<4;++i)assert(!w.constants[i]);
    assert(!ps5vk_blend_encode(0,c,VK_FALSE,&w));
    assert(!ps5vk_blend_encode(&a,c,VK_FALSE,0));
    assert(!ps5vk_blend_encode(&a,c,2,&w));
    a.blendEnable=2;assert(!ps5vk_blend_encode(&a,c,VK_FALSE,&w));
    uint32_t sx[3];
    const VkFormat formats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM};
    for(unsigned i=0;i<2;++i) {
        assert(ps5vk_color_export_state(formats[i],4,15,VK_TRUE,VK_FALSE,sx));
        assert(sx[0]==5 && sx[1]==6 && sx[2]==0);
        assert(ps5vk_color_export_state(formats[i],9,15,VK_FALSE,VK_FALSE,sx));
        assert(sx[0]==1 && sx[1]==0 && sx[2]==0);
        assert(!ps5vk_color_export_state(formats[i],9,15,VK_TRUE,VK_FALSE,sx));
        assert(!sx[0] && !sx[1] && !sx[2]);
        assert(!ps5vk_color_export_state(formats[i],0x44,15,VK_TRUE,VK_TRUE,sx));
        assert(!ps5vk_color_export_state(formats[i],0x44,0xff,VK_TRUE,VK_FALSE,sx));
        assert(ps5vk_color_export_state(formats[i],0x44,0xff,VK_TRUE,VK_TRUE,sx));
        assert(sx[0]==5 && sx[1]==6 && sx[2]==0);
        assert(ps5vk_color_export_state(formats[i],0x44,0xff,VK_FALSE,VK_TRUE,sx));
        assert(ps5vk_color_export_state(formats[i],0,0,VK_FALSE,VK_FALSE,sx));
        assert(!sx[0] && !sx[1] && !sx[2]);
        assert(!ps5vk_color_export_state(formats[i],0,15,VK_FALSE,VK_FALSE,sx));
        assert(!ps5vk_color_export_state(formats[i],9,0,VK_FALSE,VK_FALSE,sx));
        assert(!ps5vk_color_export_state(formats[i],0,0,VK_TRUE,VK_FALSE,sx));
    }
    assert(!ps5vk_color_export_state(VK_FORMAT_R32_UINT,4,15,VK_TRUE,VK_FALSE,sx));
    assert(!ps5vk_color_export_state(formats[0],4,15,2,VK_FALSE,sx));
    assert(!ps5vk_color_export_state(formats[0],4,15,VK_TRUE,2,sx));
    assert(!ps5vk_color_export_state(formats[0],4,15,VK_TRUE,VK_FALSE,0));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="ps5vk-blend-") as tmp:
            path = pathlib.Path(tmp)
            (path / "test.c").write_text(source)
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-I" + str(ROOT / "native"),
                "-I" + str(ROOT / "third_party/vulkan-headers/include"),
                str(path / "test.c"), "-o", str(path / "test"),
            ], check=True)
            subprocess.run([str(path / "test")], check=True)
