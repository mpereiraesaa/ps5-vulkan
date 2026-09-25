#ifndef PS5VK_GRAPHICS_DESCRIPTOR_PROFILE_H
#define PS5VK_GRAPHICS_DESCRIPTOR_PROFILE_H
#include <vulkan/vulkan.h>
/* Type routing only: callers must still validate visibility, ownership,
 * defined elements, buffer ranges and dynamic offsets. No capability claim. */
static inline int ps5vk_graphics_buffer_type(VkDescriptorType type)
{
    return type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
        type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
        type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
        type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
/* A sampled image read through a T#: the combined record's image half or the
 * separate SAMPLED_IMAGE record DXVK binds for every D3D11 SRV texture. */
static inline int ps5vk_graphics_sampled_image_type(VkDescriptorType type)
{
    return type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
        type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
}
static inline int ps5vk_graphics_descriptor_type(VkDescriptorType type)
{
    /* SAMPLER and SAMPLED_IMAGE are DXVK's separate forms, combined in the
     * shader with OpSampledImage; a uniform texel buffer is a Buffer<> SRV.
     * Storage texel buffers (UAVs in graphics stages) stay outside. */
    return ps5vk_graphics_buffer_type(type) ||
        ps5vk_graphics_sampled_image_type(type) ||
        type==VK_DESCRIPTOR_TYPE_SAMPLER ||
        type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
        type==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
#endif
