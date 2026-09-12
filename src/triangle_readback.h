#ifndef PS5VK_TRIANGLE_READBACK_H
#define PS5VK_TRIANGLE_READBACK_H
#include <stdint.h>
#include <stddef.h>
struct ps5vk_triangle_readback {
    uint64_t changed, bad_alpha, bad_sum;
    unsigned minimum[3], maximum[3];
};
/* Aggregate oracle for experiments/graphics/triangle.pipe only. Swizzle/order
 * independent, so it cannot prove screen position or per-pixel barycentrics. */
struct ps5vk_triangle_readback ps5vk_triangle_scan(const uint32_t *, size_t, uint32_t sentinel);
int ps5vk_triangle_readback_valid(const struct ps5vk_triangle_readback *, uint32_t width, uint32_t height);
int ps5vk_triangle_coverage_valid(const struct ps5vk_triangle_readback *, uint32_t width, uint32_t height);
#endif
