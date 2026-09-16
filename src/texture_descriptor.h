#ifndef PS5VK_TEXTURE_DESCRIPTOR_H
#define PS5VK_TEXTURE_DESCRIPTOR_H
#include "vk_image.h"
#include "vk_sampler.h"
/* Build a combined T#/S# without publishing or retaining it. Caller owns
 * layout transitions, cache visibility and image/view/sampler lifetime.
 * Output is unchanged on failure. Mip/layer bounds come from the public image
 * and view contracts; unsupported descriptor combinations fail closed. */
VkResult ps5vk_texture_descriptor(VkDevice,VkImageView,VkSampler,uint32_t out[12]);
/* Build the same GFX10 image record the combined path uses, WITHOUT the sampler
 * words: eight DWORDs for a resource-only descriptor. Input attachments accept
 * only the exact resource this profile publishes and witnessed - a live,
 * same-device 2D or 2D_ARRAY view over a bound R8G8B8A8_UNORM image created for
 * INPUT_ATTACHMENT use, one mip, one sample, one to six layers and depth one -
 * so every other view, format, usage or lifetime fails closed. Image layout is
 * not part of this contract: the call takes no VkDescriptorImageInfo, so the
 * layout an input attachment is consumed in is validated by the descriptor
 * write and by native consumption, not here. Output is unchanged on failure and
 * no sampler is read or written. */
VkResult ps5vk_image_resource_descriptor(VkDevice,VkImageView,uint32_t out[8]);
#endif
