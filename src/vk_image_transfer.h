#ifndef PS5VK_IMAGE_TRANSFER_H
#define PS5VK_IMAGE_TRANSFER_H
#include "vk_command.h"

/* Vulkan 1.0 image copy / colour clear domain.
 *
 * Only the role this profile can describe truthfully is implemented: an RGBA8
 * image whose usage is drawn from TRANSFER_SRC/TRANSFER_DST alone, which the
 * driver backs with the padded linear layout used by the upload path (256-byte
 * row pitch). The tiled colour-attachment layout has no linear addressing and
 * is refused here rather than faked with a linear memset.
 *
 * vkCmdClearDepthStencilImage and vkCmdClearAttachments are exposed and fully
 * validated but fail closed: 64KB_Z_X has no pixel addressing in this codebase,
 * and a mid-render-pass attachment clear would need a DCB clear path that does
 * not exist yet.
 *
 * Effects execute in start_submission when the frontend segment reaches the
 * head, never at record time; the destination allocation range is flushed
 * through the memory backend after each driver-originated write. */
VkBool32 ps5vk_image_transfer_operation(enum ps5vk_operation_type type);
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
