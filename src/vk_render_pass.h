#ifndef PS5VK_RENDER_PASS_H
#define PS5VK_RENDER_PASS_H
#include "vk_internal.h"

/* Initial one-subpass profile; owned data, never retained create-info pointers.
 * A graphics-capable backend must be enabled before these objects are usable. */
struct VkRenderPass_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    unsigned pending;
    uint32_t attachment_count, dependency_count;
    VkAttachmentDescription attachments[2];
    VkAttachmentReference color, depth;
    VkSubpassDependency dependencies[2];
};
#endif
