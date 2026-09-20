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
static inline int ps5vk_graphics_descriptor_type(VkDescriptorType type)
{
    return ps5vk_graphics_buffer_type(type) ||
        type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
        type==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
#endif
