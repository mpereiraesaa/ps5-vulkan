#ifndef PS5VK_TEXTURE_LAYOUT_H
#define PS5VK_TEXTURE_LAYOUT_H
#include <stdint.h>
#include <vulkan/vulkan_core.h>
struct ps5vk_texture_layout {
    uint32_t row_pitch;
    uint64_t bytes, alignment, slice_pitch;
};
#define PS5VK_MAX_TEXTURE_MIP_LEVELS 16u
#ifndef PS5VK_ENABLE_MIPMAP_CANDIDATE
#define PS5VK_ENABLE_MIPMAP_CANDIDATE 0
#endif
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
#endif
