#include "vertex_format_probe.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    static const char *names[PS5VK_VERTEX_FORMAT_CASES] = {
        "r32-sint", "rg32-sint", "rgb32-sint", "rgba32-sint",
        "r32-uint", "rg32-uint", "rgb32-uint", "rgba32-uint",
    };
    struct ps5vk_vertex_format_case c;
    for (unsigned i = 0; i < PS5VK_VERTEX_FORMAT_CASES; ++i) {
        assert(!ps5vk_vertex_format_case(i, &c));
        assert(!strcmp(c.name, names[i]));
        assert(c.components == i % 4 + 1);
        assert(c.numeric == (i < 4 ? PS5VK_VERTEX_PROBE_SINT : PS5VK_VERTEX_PROBE_UINT));
    }
    assert(ps5vk_vertex_format_case(PS5VK_VERTEX_FORMAT_CASES, &c));
    assert(ps5vk_vertex_format_case(0, 0));
    return 0;
}
