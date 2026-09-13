#include "sampler_core_probe.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    static const char *names[PS5VK_SAMPLER_CORE_CASES]={"mirrored-repeat",
        "transparent-black-border","opaque-black-border","opaque-white-border"};
    static const uint32_t expected[PS5VK_SAMPLER_CORE_CASES]={0xffff0000,0,0xff000000,0xffffffff};
    struct ps5vk_sampler_core_case c;
    for(unsigned i=0;i<PS5VK_SAMPLER_CORE_CASES;++i) {
        assert(!ps5vk_sampler_core_case(i,&c));assert(!strcmp(c.name,names[i]));
        assert(c.expected_bgra==expected[i]);
        assert(c.address_mode==(i?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT));
        assert(c.uv==(i?-2.0f:-0.25f));
    }
    assert(ps5vk_sampler_core_case(PS5VK_SAMPLER_CORE_CASES,&c));
    assert(ps5vk_sampler_core_case(0,NULL));
}
