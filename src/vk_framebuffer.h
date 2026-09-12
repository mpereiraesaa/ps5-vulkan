#ifndef PS5VK_FRAMEBUFFER_H
#define PS5VK_FRAMEBUFFER_H
#include "vk_render_pass.h"
#include "vk_image.h"
struct VkFramebuffer_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    unsigned pending;
    uint32_t width, height, attachment_count;
    VkImageView attachments[2];
    VkFormat formats[2];
    VkSampleCountFlagBits samples[2];
    uint32_t color_attachment, depth_attachment;
};
VkBool32 ps5vk_framebuffer_compatible(VkFramebuffer framebuffer, VkRenderPass pass);
#endif
