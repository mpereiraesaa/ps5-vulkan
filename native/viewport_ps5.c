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
VkResult ps5vk_native_viewport(const VkViewport *v, const VkRect2D *s,
    const VkRect2D *area, ps5_agc_register out[PS5VK_VIEWPORT_REGISTERS])
{
    if (!out) return VK_ERROR_UNKNOWN;
    memset(out, 0, sizeof(*out) * PS5VK_VIEWPORT_REGISTERS);
    uint32_t sr, sb, ar, ab;
    /* Ordered comparisons also reject NaNs. Negative-height extensions are not
     * advertised. Vulkan minDepth > maxDepth is valid and must stay reversed. */
    if (!v || !(v->width > 0 && v->width <= 16384) || !(v->height > 0 && v->height <= 16384) ||
        !(v->x >= -32768 && v->x + v->width <= 32767) ||
        !(v->y >= -32768 && v->y + v->height <= 32767) ||
        !(v->minDepth >= 0 && v->minDepth <= 1) || !(v->maxDepth >= 0 && v->maxDepth <= 1) ||
        !rect(s, &sr, &sb) || !rect(area, &ar, &ab)) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t x = s->offset.x > area->offset.x ? (uint32_t)s->offset.x : (uint32_t)area->offset.x;
    uint32_t y = s->offset.y > area->offset.y ? (uint32_t)s->offset.y : (uint32_t)area->offset.y;
    uint32_t right = sr < ar ? sr : ar, bottom = sb < ab ? sb : ab;
    if (right < x) right = x;
    if (bottom < y) bottom = y;
    const ps5_agc_register values[PS5VK_VIEWPORT_REGISTERS] = {
        {0x10f, bits(v->width * 0.5f)}, {0x110, bits(v->x + v->width * 0.5f)},
        {0x111, bits(v->height * 0.5f)}, {0x112, bits(v->y + v->height * 0.5f)},
        {0x113, bits(v->maxDepth - v->minDepth)}, {0x114, bits(v->minDepth)},
        {0x0b4, bits(v->minDepth < v->maxDepth ? v->minDepth : v->maxDepth)},
        {0x0b5, bits(v->minDepth > v->maxDepth ? v->minDepth : v->maxDepth)},
        /* RADV gfx10 uses PA_SC_VPORT_SCISSOR_0, not GENERIC_SCISSOR. */
        {0x094, 0x80000000u | x | (y << 16)}, {0x095, right | (bottom << 16)},
    };
    memcpy(out, values, sizeof(values)); return VK_SUCCESS;
}
