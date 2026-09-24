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

int ps5vk_depth16_layout(uint32_t width, uint32_t height, struct ps5vk_depth_layout *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (width != 128u || height != 128u) return -1;
    *out = (struct ps5vk_depth_layout){width, height, width, height,
        UINT64_C(65536), UINT64_C(65536), 24u};
    return 0;
}

int ps5vk_depth_stencil_layout(uint32_t width, uint32_t height,
    struct ps5vk_depth_stencil_layout *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    struct ps5vk_depth_layout depth;
    if (ps5vk_depth_layout(width, height, &depth)) return -1;
    const uint32_t pitch = (width + 255u) & ~255u;
    const uint32_t padded = (height + 255u) & ~255u;
    const uint64_t stencil_bytes = (uint64_t)pitch * padded;
    const uint64_t alignment = 65536u;
    const uint64_t offset = (depth.bytes + alignment - 1u) & ~(alignment - 1u);
    *out = (struct ps5vk_depth_stencil_layout){depth, pitch, padded, offset,
        stencil_bytes, offset + stencil_bytes, alignment};
    return 0;
}
