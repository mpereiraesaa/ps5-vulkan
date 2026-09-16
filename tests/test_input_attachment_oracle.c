/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The subpass-input readback oracle has to be able to say more than "wrong":
 * it must accept exactly the GPU's expected transform and classify the three
 * failure shapes the strict run is looking for - subpass 1 skipped, subpass 1
 * passing the pattern through, and the transform collapsing to one value -
 * plus a single wrong pixel anywhere in the target. */
#include "input_attachment_oracle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 16, HEIGHT = 8 };

static void fill_expected(uint32_t *pixels)
{
    for (uint32_t y = 0; y < HEIGHT; ++y)
        for (uint32_t x = 0; x < WIDTH; ++x)
            pixels[y * WIDTH + x] = ps5vk_input_attachment_oracle_expected(x, y);
}

static void fill(uint32_t *pixels, uint32_t value)
{
    for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; ++i) pixels[i] = value;
}

int main(void)
{
    uint32_t *pixels = malloc(sizeof(uint32_t) * WIDTH * HEIGHT);
    assert(pixels);
    struct ps5vk_input_attachment_oracle_result result;

    /* The expected transform is accepted, over every pixel, and the count says
     * so rather than merely the verdict. */
    fill_expected(pixels);
    result = ps5vk_input_attachment_oracle_verify(pixels, WIDTH, HEIGHT);
    assert(result.verdict == PS5VK_INPUT_ATTACHMENT_ORACLE_OK);
    assert(result.matched == (unsigned long)WIDTH * HEIGHT && result.total == result.matched);

    /* One wrong pixel is a mismatch, and it is located. */
    pixels[3u * WIDTH + 5] ^= 0x000000ffu;
    result = ps5vk_input_attachment_oracle_verify(pixels, WIDTH, HEIGHT);
    assert(result.verdict == PS5VK_INPUT_ATTACHMENT_ORACLE_MISMATCH);
    assert(result.matched == (unsigned long)WIDTH * HEIGHT - 1);
    assert(result.first_x == 5 && result.first_y == 3);

    /* The three failure shapes are named, not lumped together. */
    fill(pixels, PS5VK_INPUT_ATTACHMENT_ORACLE_BACKGROUND);
    assert(ps5vk_input_attachment_oracle_verify(pixels, WIDTH, HEIGHT).verdict ==
        PS5VK_INPUT_ATTACHMENT_ORACLE_SKIPPED);
    for (uint32_t y = 0; y < HEIGHT; ++y)
        for (uint32_t x = 0; x < WIDTH; ++x)
            pixels[y * WIDTH + x] = ps5vk_input_attachment_oracle_pattern(x, y);
    assert(ps5vk_input_attachment_oracle_verify(pixels, WIDTH, HEIGHT).verdict ==
        PS5VK_INPUT_ATTACHMENT_ORACLE_PASS_THROUGH);
    fill(pixels, UINT32_C(0xff204080));
    assert(ps5vk_input_attachment_oracle_verify(pixels, WIDTH, HEIGHT).verdict ==
        PS5VK_INPUT_ATTACHMENT_ORACLE_CONSTANT);

    /* A degenerate caller is refused rather than read. */
    assert(ps5vk_input_attachment_oracle_verify(NULL, WIDTH, HEIGHT).verdict ==
        PS5VK_INPUT_ATTACHMENT_ORACLE_MISMATCH);
    assert(ps5vk_input_attachment_oracle_verify(pixels, 0, HEIGHT).verdict ==
        PS5VK_INPUT_ATTACHMENT_ORACLE_MISMATCH);
    assert(ps5vk_input_attachment_oracle_verify(pixels, WIDTH, 0).verdict ==
        PS5VK_INPUT_ATTACHMENT_ORACLE_MISMATCH);

    /* The pattern and the transform are reasoned about, not just observed: the
     * transform never reproduces its input (so pass-through cannot pass) and
     * never equals the background (so a skipped subpass cannot), and the
     * pattern is not constant (so a constant output cannot). */
    assert(ps5vk_input_attachment_oracle_pattern(0, 0) !=
        ps5vk_input_attachment_oracle_pattern(1, 0));
    assert(ps5vk_input_attachment_oracle_pattern(0, 0) !=
        ps5vk_input_attachment_oracle_pattern(0, 1));
    for (uint32_t byte = 0; byte < 256; ++byte) {
        const uint32_t pattern = UINT32_C(0xff000000) | (byte << 16) |
            ((byte * 7u) << 8) | (byte ^ 0x5au);
        const uint32_t transformed = ps5vk_input_attachment_oracle_transform(pattern);
        assert(transformed != pattern);
        assert(transformed != PS5VK_INPUT_ATTACHMENT_ORACLE_BACKGROUND);
        /* ...and the transform is its own classification: reading it again
         * would be a different transform, so a double read cannot look
         * correct. */
        assert(ps5vk_input_attachment_oracle_transform(transformed) != pattern);
    }
    /* The expected image holds no two identical rows or columns, so a rolled
     * or truncated readback cannot coincide with it. */
    for (uint32_t y = 0; y + 1 < HEIGHT; ++y)
        for (uint32_t x = 0; x + 1 < WIDTH; ++x) {
            assert(ps5vk_input_attachment_oracle_expected(x, y) !=
                ps5vk_input_attachment_oracle_expected(x + 1, y));
            assert(ps5vk_input_attachment_oracle_expected(x, y) !=
                ps5vk_input_attachment_oracle_expected(x, y + 1));
        }
    free(pixels);
    puts("Input-attachment oracle: expected transform accepted, pass-through/skipped/constant named");
    return 0;
}
