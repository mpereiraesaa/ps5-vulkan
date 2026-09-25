#ifndef PS5VK_DYNAMIC_RENDERING_H
#define PS5VK_DYNAMIC_RENDERING_H
#include "vk_render_pass.h"

/* VK_KHR_dynamic_rendering (DXVK262-T10): the version-1 description of the
 * single-subpass render pass equivalent to one VkRenderingInfo. Colour
 * attachments come first, in the order the begin info names them, then the
 * depth/stencil attachment; every layout is the one the begin info names,
 * unchanged across the instance. `views` and `clears` are indexed like
 * `attachments`. Nothing points into the caller's structures. */
struct ps5vk_dynamic_rendering_shape {
    uint32_t attachment_count, color_count;
    VkAttachmentDescription attachments[PS5VK_MAX_COLOR_ATTACHMENTS + 1];
    VkAttachmentReference color[PS5VK_MAX_COLOR_ATTACHMENTS];
    VkAttachmentReference depth;
    VkImageView views[PS5VK_MAX_COLOR_ATTACHMENTS + 1];
    VkClearValue clears[PS5VK_MAX_COLOR_ATTACHMENTS + 1];
};
/* VK_SUCCESS with the shape filled, VK_ERROR_FEATURE_NOT_PRESENT for a shape
 * this profile does not execute, VK_ERROR_UNKNOWN for a malformed one. */
VkResult ps5vk_dynamic_rendering_pass(VkDevice d, const VkRenderingInfo *info,
    struct ps5vk_dynamic_rendering_shape *out);
#endif
