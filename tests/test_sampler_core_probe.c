#include "sampler_core_probe.h"
#include <assert.h>
#include <string.h>
static float mirror_once(float coord)
{
    if(coord<0.0f)coord=-coord;
    return coord>1.0f?1.0f:coord;
}
static float texel_weight(float coord,VkFilter filter)
{
    if(filter==VK_FILTER_NEAREST)return coord>=0.5f?1.0f:0.0f;
    float weight=coord*2.0f-0.5f;
    return weight<0.0f?0.0f:(weight>1.0f?1.0f:weight);
}
static uint32_t mirror_reference(const struct ps5vk_sampler_core_case *c)
{
    /* The source texture is RGBA8; the readback target packs BGRA8 bytes. */
    const uint32_t source[4]={UINT32_C(0xffff0000),UINT32_C(0xff00ff00),
                              UINT32_C(0xff0000ff),UINT32_C(0xffff0000)};
    float u=c->mirror_axis==1?mirror_once(c->uv):c->uv;
    float v=c->mirror_axis==2?mirror_once(c->uv_v):c->uv_v;
    float x=texel_weight(u,c->mag_filter),y=texel_weight(v,c->mag_filter);
    uint32_t result=0;
    for(unsigned channel=0;channel<4;++channel) {
        unsigned shift=8u*channel;
        float value=(float)((source[0]>>shift)&255u)*(1-x)*(1-y)+
                    (float)((source[1]>>shift)&255u)*x*(1-y)+
                    (float)((source[2]>>shift)&255u)*(1-x)*y+
                    (float)((source[3]>>shift)&255u)*x*y;
        result|=(uint32_t)(value+0.5f)<<shift;
    }
    return result;
}
int main(void)
{
    enum { OLD_CASES = PS5VK_SAMPLER_MIRROR_FIRST_CASE };
    static const char *names[OLD_CASES]={"mirrored-repeat",
        "transparent-black-border","opaque-black-border","opaque-white-border",
        "nearest-center-control","linear-magnification","nearest-minification-control",
        "linear-minification"};
    static const uint32_t expected[OLD_CASES]={0xffff0000,0,0xff000000,
        0xffffffff,0xff000000,0xff808080,0xff000000,0xff808080};
    struct ps5vk_sampler_core_case c;
    for(unsigned i=0;i<OLD_CASES;++i) {
        assert(!ps5vk_sampler_core_case(i,&c));assert(!strcmp(c.name,names[i]));
        assert(c.expected_bgra==expected[i]);
        assert(c.address_mode==(i==0?VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
            (i<4?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
            (i<6?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:VK_SAMPLER_ADDRESS_MODE_REPEAT))));
        assert(c.uv==(i==0?-0.25f:(i<4?-2.0f:0.5f)));
        assert(c.mag_filter==((i==5 || i==6)?VK_FILTER_LINEAR:VK_FILTER_NEAREST));
        assert(c.min_filter==(i==7?VK_FILTER_LINEAR:VK_FILTER_NEAREST));
        assert(c.checkerboard==(i>=4));assert(c.minification==(i>=6));
    }
    for(unsigned i=PS5VK_SAMPLER_MIRROR_FIRST_CASE;i<PS5VK_SAMPLER_CORE_CASES;++i) {
        assert(!ps5vk_sampler_core_case(i,&c));
        assert(c.address_mode==VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE);
        assert(c.mirror_axis==(i<14?1u:2u));
        assert(c.mag_filter==((i-PS5VK_SAMPLER_MIRROR_FIRST_CASE)%6<3?
            VK_FILTER_NEAREST:VK_FILTER_LINEAR));
        assert(c.min_filter==c.mag_filter && !c.checkerboard && !c.minification);
        assert(c.expected_bgra==mirror_reference(&c));
        assert(ps5vk_sampler_core_color_near(c.expected_bgra,c.expected_bgra,0));
    }
    assert(ps5vk_sampler_core_color_near(UINT32_C(0xff007f80),UINT32_C(0xff008080),1));
    assert(!ps5vk_sampler_core_color_near(UINT32_C(0xff007e80),UINT32_C(0xff008080),1));
    assert(ps5vk_sampler_core_case(PS5VK_SAMPLER_CORE_CASES,&c));
    assert(ps5vk_sampler_core_case(0,NULL));
}
