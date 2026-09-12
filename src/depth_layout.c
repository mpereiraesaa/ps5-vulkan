#include "depth_layout.h"
#include <string.h>
int ps5vk_depth_layout(uint32_t width, uint32_t height, struct ps5vk_depth_layout *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!width || !height || width > 16384 || height > 16384) return -1;
    uint32_t pitch = (width + 127u) & ~127u;
    uint32_t padded = (height + 127u) & ~127u;
    *out = (struct ps5vk_depth_layout){width, height, pitch, padded,
        (uint64_t)pitch * padded * 4u, 65536u, 24u};
    return 0;
}
