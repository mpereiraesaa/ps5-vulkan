#include "vertex_format_probe.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    static const char *names[PS5VK_VERTEX_FORMAT_CASES] = {
        "r32-sint", "rg32-sint", "rgb32-sint", "rgba32-sint",
        "r32-uint", "rg32-uint", "rgb32-uint", "rgba32-uint",
        "rgba8-unorm", "bgra8-unorm", "a2b10g10r10-unorm",
    };
    struct ps5vk_vertex_format_case c;
    for (unsigned i = 0; i < PS5VK_VERTEX_FORMAT_CASES; ++i) {
        assert(!ps5vk_vertex_format_case(i, &c));
        assert(!strcmp(c.name, names[i]));
        assert(c.components == (i < 8 ? i % 4 + 1 : 4));
        assert(c.numeric == (i < 4 ? PS5VK_VERTEX_PROBE_SINT :
            (i < 8 ? PS5VK_VERTEX_PROBE_UINT : PS5VK_VERTEX_PROBE_UNORM)));
        assert(c.raw_word == (i < 8 ? UINT32_MAX :
            (i < 10 ? UINT32_C(0xffaa5511) : UINT32_C(0xbffaa955))));
        if (i == 8) assert(c.expected[0] < c.expected[2]);
        if (i == 9) assert(c.expected[0] > c.expected[2]);
        if (i == 10) assert(c.expected[0] < c.expected[1] &&
            c.expected[1] < c.expected[2] && c.expected[3] == c.expected[1]);
    }
    assert(ps5vk_vertex_format_case(PS5VK_VERTEX_FORMAT_CASES, &c));
    assert(ps5vk_vertex_format_case(0, 0));
    return 0;
}
