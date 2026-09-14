#ifndef PS5VK_TEXTURE_DESCRIPTOR_H
#define PS5VK_TEXTURE_DESCRIPTOR_H
#include "vk_image.h"
#include "vk_sampler.h"
/* Build a combined T#/S# without publishing or retaining it. Caller owns
 * layout transitions, cache visibility and image/view/sampler lifetime.
 * Output is unchanged on failure. Mip/layer bounds come from the public image
 * and view contracts; unsupported descriptor combinations fail closed. */
VkResult ps5vk_texture_descriptor(VkDevice,VkImageView,VkSampler,uint32_t out[12]);
#endif
