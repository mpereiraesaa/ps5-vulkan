#ifndef PS5VK_IMAGE_TRANSFER_H
#define PS5VK_IMAGE_TRANSFER_H
#include "vk_command.h"
#include <string.h>

/* Vulkan 1.0 image copy / colour clear domain.
 *
 * Only the role this profile can describe truthfully is implemented: an RGBA8
 * image whose usage is drawn from TRANSFER_SRC/TRANSFER_DST alone, which the
 * driver backs with the padded linear layout used by the upload path (256-byte
 * row pitch). The tiled colour-attachment layout has no linear addressing and
 * is refused here rather than faked with a linear memset.
 *
 * vkCmdClearDepthStencilImage executes for the whole subresource of a one-
 * sample D32_SFLOAT target that also carries transfer-destination usage. A
 * constant depth value is the same 32-bit word in every texel, so the uniform
 * DWORD fill the render pass already uses for its depth load-op clear writes
 * exactly the right image without the 64KB_Z_X pixel equations this codebase
 * still does not claim; partial ranges, rectangles, stencil aspects and
 * multisample images therefore remain fail-closed. vkCmdClearAttachments is
 * exposed and fully validated but fail closed: a mid-render-pass attachment
 * clear would need a DCB clear path that does not exist yet.
 *
 * Effects execute in start_submission when the frontend segment reaches the
 * head, never at record time; the destination allocation range is flushed
 * through the memory backend after each driver-originated write. */
VkBool32 ps5vk_image_transfer_operation(enum ps5vk_operation_type type);

/* Which executor owns a recorded image operation: the pure transfer role's row
 * memcpy (TRANSFER) or the linear frontend's padded-linear work, which now
 * includes the colour-attachment readback copy into the staging image (LINEAR).
 * Exactly one domain owns an operation, so the two executors cannot both act on
 * it. */
enum ps5vk_image_domain {
    PS5VK_IMAGE_DOMAIN_NONE,
    PS5VK_IMAGE_DOMAIN_TRANSFER,
    PS5VK_IMAGE_DOMAIN_LINEAR
};
enum ps5vk_image_domain ps5vk_image_domain(const struct ps5vk_operation *operation);

/* The depth role vkCmdClearDepthStencilImage accepts: a one-sample D32_SFLOAT
 * 2D target carrying the transfer-destination usage the clear consumes. The
 * depth/stencil attachment usage may accompany it but is not required, because
 * Vulkan asks only for TRANSFER_DST on a cleared image. mipLevels and
 * arrayLayers are already forced to one by ps5vk_native_image_requirements for
 * every D32 image. Inline so the recorder, the submit-time gate and the native
 * emitter share one definition instead of three. */
static inline VkBool32 ps5vk_depth_clear_image(VkImage image)
{
    return image && image->info.format == VK_FORMAT_D32_SFLOAT &&
        image->info.imageType == VK_IMAGE_TYPE_2D &&
        image->info.samples == VK_SAMPLE_COUNT_1_BIT &&
        image->info.tiling == VK_IMAGE_TILING_OPTIMAL &&
        image->info.mipLevels == 1 && image->info.arrayLayers == 1 &&
        image->info.extent.depth == 1 &&
        (image->info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? VK_TRUE : VK_FALSE;
}

/* A D32_SFLOAT depth value in [0,1] as the 32-bit word the surface stores.
 * The comparison rejects NaN, and the render pass load-op clear encodes the
 * identical bits, so both clear paths agree byte for byte. */
static inline VkBool32 ps5vk_depth_clear_word(float depth, uint32_t *out)
{
    if (!out || !(depth >= 0.0f && depth <= 1.0f)) return VK_FALSE;
    memcpy(out, &depth, sizeof(*out));
    return VK_TRUE;
}
/* True when this recorded operation is frontend work of the pure transfer
 * role. The render-target readback prelude/postlude keeps its GPU path. */
VkBool32 ps5vk_image_linear_operation(const struct ps5vk_operation *operation);
VkResult ps5vk_image_transfer_execute(VkDevice device, const struct ps5vk_operation *operation);
VkResult ps5vk_image_linear_execute(VkDevice device, const struct ps5vk_operation *operation);
/* Re-validate a recorded image operation against the live device state; used by
 * submit-time command validation and again before execution at the head. */
VkResult ps5vk_image_transfer_validate(VkDevice device, const struct ps5vk_operation *operation);
VkResult ps5vk_image_linear_validate(VkDevice device, const struct ps5vk_operation *operation);
/* Record-time region gate for the padded-linear transfer role: validates one
 * base-level RGBA8 area between the buffer and the image in either direction
 * before any operation is appended. */
VkResult ps5vk_image_linear_region_validate(VkImage image, const VkBufferImageCopy *region,
    VkDeviceSize buffer_bytes, VkDeviceSize image_bytes);

#endif
