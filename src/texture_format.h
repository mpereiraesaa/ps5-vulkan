#ifndef PS5VK_TEXTURE_FORMAT_H
#define PS5VK_TEXTURE_FORMAT_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* Internal sampled-image encoding.  Entries can be known from a compatible
 * implementation without yet being advertised by this Vulkan driver. */
struct ps5vk_texture_format {
    VkFormat format;
    uint32_t bytes_per_texel;
    uint32_t descriptor_format_word;
    uint8_t selectors[4];
    VkBool32 linear_filter_candidate;
    VkBool32 validated;
};

const struct ps5vk_texture_format *ps5vk_texture_format_lookup(VkFormat format);
VkBool32 ps5vk_texture_format_supported(VkFormat format);

#endif
