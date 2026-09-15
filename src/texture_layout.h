#ifndef PS5VK_TEXTURE_LAYOUT_H
#define PS5VK_TEXTURE_LAYOUT_H
#include <stdint.h>
#include <vulkan/vulkan_core.h>
struct ps5vk_texture_layout {
    uint32_t row_pitch;
    uint64_t bytes, alignment, slice_pitch;
};
#define PS5VK_MAX_TEXTURE_MIP_LEVELS 16u
struct ps5vk_texture_mip_level {
    uint64_t offset;
    uint32_t row_pitch, storage_width, storage_height;
};
struct ps5vk_texture_mip_layout {
    struct ps5vk_texture_mip_level levels[PS5VK_MAX_TEXTURE_MIP_LEVELS];
    uint32_t level_count, storage_layers;
    uint64_t layer_stride, bytes, alignment;
};
/* GFX1013 padded-linear sampled layout adapted from ps5-opengl. Levels are
 * packed in descending mip order (the smallest level is at offset zero), and
 * a complete mip chain is repeated for every array layer/cube face or base
 * 3D z-slice. storage_width/height use ceil division exactly as the native
 * descriptor contract requires; Vulkan copy bounds still use floor mip sizes. */
int ps5vk_texture_mip_layout_for_slices(VkFormat,uint32_t,uint32_t,uint32_t,
    uint32_t,struct ps5vk_texture_mip_layout *);
/* Internal one-level sampling layout, not a Vulkan linear-tiling promise.
 * Xash3D/GFX10 sampling profile: 256-byte base and row alignment. */
int ps5vk_texture_layout_for_format(VkFormat,uint32_t,uint32_t,
    struct ps5vk_texture_layout *);
/* One-level padded-linear sampled storage. A slice is one array layer, cube
 * face or 3D z plane. */
int ps5vk_texture_layout_for_slices(VkFormat,uint32_t,uint32_t,uint32_t,
    struct ps5vk_texture_layout *);
/* Compatibility helper for the existing RGBA8 transfer-only implementation. */
int ps5vk_texture_layout(uint32_t,uint32_t,struct ps5vk_texture_layout *);

/* The same one-level padded-linear arithmetic as a header-only inline, for the
 * translation units that must not drag the chain builder into their link set
 * (vk_memory.c answers vkGetImageSubresourceLayout, the queue group performs the
 * staging readback). tests/test_image_copy_clear.c and tests/test_texture_layout.c
 * assert that this formula and ps5vk_texture_layout_for_format agree. */
static inline int ps5vk_texture_row_layout(uint32_t bytes_per_texel, uint32_t width,
    uint32_t height, uint32_t *row_pitch, uint64_t *bytes)
{
    if (!row_pitch || !bytes || !bytes_per_texel || !width || !height ||
        width > 16384 || height > 16384) return -1;
    const uint64_t row = ((uint64_t)width * bytes_per_texel + 255u) & ~UINT64_C(255);
    if (row > UINT32_MAX || (uint64_t)height > UINT64_MAX / row) return -1;
    *row_pitch = (uint32_t)row;
    *bytes = row * height;
    return 0;
}
#endif
