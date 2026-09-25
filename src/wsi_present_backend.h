#ifndef PS5VK_WSI_PRESENT_BACKEND_H
#define PS5VK_WSI_PRESENT_BACKEND_H

#include "vulkan/vulkan.h"
#include <stdint.h>

/* Internal bridge between Vulkan WSI and the native display path. A native
 * open pins exactly two BGRA8 UNORM 1920x1080 color attachments until close.
 * The images must be bound at aligned addresses 64 MiB apart. */
struct ps5vk_wsi_present;

VkBool32 ps5vk_wsi_present_available(void);
VkResult ps5vk_wsi_present_open(VkDevice device, const VkImage images[2],
                               struct ps5vk_wsi_present **out);
/* Synchronous: success means the GPU fence and matching flip event completed.
 * A slot displayed by the previous frame becomes writable only then. */
VkResult ps5vk_wsi_present_frame(struct ps5vk_wsi_present *present,
                                uint32_t slot, uint64_t token);
/* Close releases the image pins on success. */
VkResult ps5vk_wsi_present_close(struct ps5vk_wsi_present *present);

#endif
