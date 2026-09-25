#include "viewport_ps5.h"
#include <string.h>

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }
static int rect(const VkRect2D *r, uint32_t *right, uint32_t *bottom)
{
    if (!r || r->offset.x < 0 || r->offset.y < 0 || r->offset.x > 32767 || r->offset.y > 32767 ||
        r->extent.width > 32767u - (uint32_t)r->offset.x ||
        r->extent.height > 32767u - (uint32_t)r->offset.y) return 0;
    *right = (uint32_t)r->offset.x + r->extent.width;
    *bottom = (uint32_t)r->offset.y + r->extent.height;
    return 1;
}
VkResult ps5vk_native_viewport_bank(uint32_t index, const VkViewport *v, const VkRect2D *s,
    const VkRect2D *area, ps5_agc_register out[PS5VK_VIEWPORT_REGISTERS])
{
    if (!out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out) * PS5VK_VIEWPORT_REGISTERS);
    if (index >= PS5VK_VIEWPORT_BANKS) return VK_ERROR_UNKNOWN;
    uint32_t sr, sb, ar, ab;
    /* Ordered comparisons also reject NaNs. A negative height is the
     * VK_KHR_maintenance1 y-flip, which the front end admits only on a device
     * that enabled it: the same scale/offset formula then encodes a negative
     * YSCALE with YOFFSET = y + height/2, and both y and y + height must lie in
     * the viewport bounds. Vulkan minDepth > maxDepth is valid and must stay
     * reversed. */
    if (!v || !(v->width > 0 && v->width <= 16384) ||
        !((v->height > 0 && v->height <= 16384) || (v->height < 0 && v->height >= -16384)) ||
        !(v->x >= -32768 && v->x + v->width <= 32767) ||
        !(v->y >= -32768 && v->y <= 32767 &&
          v->y + v->height >= -32768 && v->y + v->height <= 32767) ||
        !(v->minDepth >= 0 && v->minDepth <= 1) || !(v->maxDepth >= 0 && v->maxDepth <= 1) ||
        !rect(s, &sr, &sb) || !rect(area, &ar, &ab)) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t x = s->offset.x > area->offset.x ? (uint32_t)s->offset.x : (uint32_t)area->offset.x;
    uint32_t y = s->offset.y > area->offset.y ? (uint32_t)s->offset.y : (uint32_t)area->offset.y;
    uint32_t right = sr < ar ? sr : ar, bottom = sb < ab ? sb : ab;
    if (right < x) right = x;
    if (bottom < y) bottom = y;
    /* Bank strides from the pinned gfx10 schema: six PA_CL_VPORT words, two
     * ZMIN/ZMAX words and two scissor words per viewport index. */
    const uint32_t vport = 0x10f + 6u * index, zrange = 0x0b4 + 2u * index,
        scissor = 0x094 + 2u * index;
    const ps5_agc_register values[PS5VK_VIEWPORT_REGISTERS] = {
        {vport, bits(v->width * 0.5f)}, {vport + 1, bits(v->x + v->width * 0.5f)},
        {vport + 2, bits(v->height * 0.5f)}, {vport + 3, bits(v->y + v->height * 0.5f)},
        {vport + 4, bits(v->maxDepth - v->minDepth)}, {vport + 5, bits(v->minDepth)},
        {zrange, bits(v->minDepth < v->maxDepth ? v->minDepth : v->maxDepth)},
        {zrange + 1, bits(v->minDepth > v->maxDepth ? v->minDepth : v->maxDepth)},
        /* RADV gfx10 uses PA_SC_VPORT_SCISSOR_n, not GENERIC_SCISSOR. */
        {scissor, 0x80000000u | x | (y << 16)}, {scissor + 1, right | (bottom << 16)},
    };
    memcpy(out, values, sizeof(values)); return VK_SUCCESS;
}
VkResult ps5vk_native_viewport(const VkViewport *v, const VkRect2D *s,
    const VkRect2D *area, ps5_agc_register out[PS5VK_VIEWPORT_REGISTERS])
{
    return ps5vk_native_viewport_bank(0, v, s, area, out);
}
