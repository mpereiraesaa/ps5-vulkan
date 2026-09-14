#include "vertex_format_probe.h"
#include "graphics_formats.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    struct ps5vk_vertex_format_case cases[PS5VK_VERTEX_FORMAT_CASES];
    for (unsigned i = 0; i < PS5VK_VERTEX_FORMAT_CASES; ++i) {
        struct ps5vk_vertex_format_case *c=&cases[i];
        assert(!ps5vk_vertex_format_case(i,c));
        assert(c->name && *c->name && c->bytes && c->bytes<=sizeof(c->raw));
        assert(c->components>=1 && c->components<=4);
        struct ps5vk_vertex_format format=ps5vk_vertex_format_info(c->format);
        assert(format.bytes==c->bytes && format.components==c->components);
        enum ps5vk_vertex_numeric numeric=c->numeric==PS5VK_VERTEX_PROBE_SINT ?
            PS5VK_VERTEX_NUMERIC_SINT : c->numeric==PS5VK_VERTEX_PROBE_UINT ?
            PS5VK_VERTEX_NUMERIC_UINT : PS5VK_VERTEX_NUMERIC_FLOAT;
        assert(format.numeric==numeric);
        for(unsigned j=0;j<i;++j) {
            assert(cases[j].format!=c->format);
            assert(strcmp(cases[j].name,c->name));
        }
    }
    assert(!strcmp(cases[11].name,"r8-unorm") && cases[11].bytes==1 &&
        cases[11].expected.f[3]==1.0f);
    assert(!strcmp(cases[22].name,"a8b8g8r8-unorm") &&
        cases[22].raw[0]==0x11 && cases[22].raw[3]==0xff);
    assert(!strcmp(cases[40].name,"rgba16-sfloat") && cases[40].bytes==8 &&
        cases[40].expected.f[2]==2.5f);
    assert(ps5vk_vertex_format_case(PS5VK_VERTEX_FORMAT_CASES,&cases[0]));
    assert(ps5vk_vertex_format_case(0,0));
    return 0;
}
