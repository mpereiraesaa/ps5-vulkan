/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The subpass-input readback oracle's arithmetic: see
 * input_attachment_oracle.h. Nothing here touches a device, which is what lets
 * the strict run's expected numbers be produced and tested on the host. */
#include "input_attachment_oracle.h"

uint32_t ps5vk_input_attachment_oracle_pattern(uint32_t x, uint32_t y)
{
    /* Three independent byte ramps: the red byte climbs along x, the green
     * byte along y, and the blue byte mixes both, so no two rows or columns of
     * the target repeat and a truncated or shifted readback cannot coincide
     * with the expected one. */
    const uint32_t red = (x * 17u + y * 3u) & 0xffu;
    const uint32_t green = (y * 29u + 7u) & 0xffu;
    const uint32_t blue = (x * 5u) ^ (y * 11u);
    return UINT32_C(0xff000000) | (red << 16) | (green << 8) | (blue & 0xffu);
}

uint32_t ps5vk_input_attachment_oracle_transform(uint32_t pattern)
{
    /* Read the three bytes, permute them, invert two:
     *   r' = 255 - g, g' = b, b' = 255 - r, a' = a
     * A transform that never leaves a byte in place cannot reproduce its input,
     * and inverting two of the three bytes means it cannot collapse to the
     * single 0xff101010 word the target is cleared to either. */
    const uint32_t red = (pattern >> 16) & 0xffu;
    const uint32_t green = (pattern >> 8) & 0xffu;
    const uint32_t blue = pattern & 0xffu;
    return UINT32_C(0xff000000) | ((0xffu - green) << 16) |
        (blue << 8) | (0xffu - red);
}

uint32_t ps5vk_input_attachment_oracle_expected(uint32_t x, uint32_t y)
{
    return ps5vk_input_attachment_oracle_transform(
        ps5vk_input_attachment_oracle_pattern(x, y));
}

struct ps5vk_input_attachment_oracle_result ps5vk_input_attachment_oracle_verify(
    const uint32_t *pixels, uint32_t width, uint32_t height)
{
    struct ps5vk_input_attachment_oracle_result result = {
        .verdict = PS5VK_INPUT_ATTACHMENT_ORACLE_MISMATCH, .total = 0,
        .first_x = 0, .first_y = 0};
    if (!pixels || !width || !height) return result;
    const unsigned long total = (unsigned long)width * (unsigned long)height;
    unsigned long matched = 0, background = 0, pattern = 0;
    uint32_t first = pixels[0];
    int constant = 1;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t pixel = pixels[(size_t)y * width + x];
            if (pixel == ps5vk_input_attachment_oracle_expected(x, y)) ++matched;
            if (pixel == PS5VK_INPUT_ATTACHMENT_ORACLE_BACKGROUND) ++background;
            if (pixel == ps5vk_input_attachment_oracle_pattern(x, y)) ++pattern;
            if (pixel != first) constant = 0;
        }
    result.total = total;
    result.matched = matched;
    if (matched == total) {
        result.verdict = PS5VK_INPUT_ATTACHMENT_ORACLE_OK;
        return result;
    }
    /* The order matters: the shapes are named before the generic mismatch, and
     * a readback that is entirely one of them is reported as that shape. */
    if (background == total) result.verdict = PS5VK_INPUT_ATTACHMENT_ORACLE_SKIPPED;
    else if (pattern == total) result.verdict = PS5VK_INPUT_ATTACHMENT_ORACLE_PASS_THROUGH;
    else if (constant) result.verdict = PS5VK_INPUT_ATTACHMENT_ORACLE_CONSTANT;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            if (pixels[(size_t)y * width + x] != ps5vk_input_attachment_oracle_expected(x, y)) {
                result.first_x = x; result.first_y = y;
                return result;
            }
    return result;
}
