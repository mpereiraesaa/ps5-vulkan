#ifndef PS5VK_SYNC2_H
#define PS5VK_SYNC2_H

#include "vk_command.h"

/* Internal converters for the bounded DXVK 2.6.2 image dependency and
 * buffer-to-image copy shapes. No public synchronization2 route is exposed. */
void ps5vk_cmd_pipeline_barrier2_bounded(VkCommandBuffer command,
                                          const VkDependencyInfo *dependency);
void ps5vk_cmd_copy_buffer_to_image2_bounded(VkCommandBuffer command,
                                              const VkCopyBufferToImageInfo2 *copy);

#endif
