#include "sampler_core_probe.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    static const char *names[PS5VK_SAMPLER_CORE_CASES]={"mirrored-repeat",
        "transparent-black-border","opaque-black-border","opaque-white-border",
        "nearest-center-control","linear-magnification","nearest-minification-control",
        "linear-minification"};
    static const uint32_t expected[PS5VK_SAMPLER_CORE_CASES]={0xffff0000,0,0xff000000,
        0xffffffff,0xff000000,0xff808080,0xff000000,0xff808080};
    struct ps5vk_sampler_core_case c;
    for(unsigned i=0;i<PS5VK_SAMPLER_CORE_CASES;++i) {
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
    assert(ps5vk_sampler_core_case(PS5VK_SAMPLER_CORE_CASES,&c));
    assert(ps5vk_sampler_core_case(0,NULL));
}
