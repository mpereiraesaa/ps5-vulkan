#ifndef PS5VK_TARGETS_PS5_H
#define PS5VK_TARGETS_PS5_H
#include "vk_image.h"
#include "ps5_color_target.h"
#include "ps5_depth_target.h"
/* Prepared target registers only. Does not clear, transition, bind or submit. */
struct ps5vk_target_registers {
    ps5_agc_register registers[PS5_DEPTH_REGISTER_COUNT];
    uint32_t count;
};
VkResult ps5vk_native_target(VkDevice, VkImageView,
    const ps5_agc_register color_defaults[PS5_COLOR_REGISTER_COUNT], struct ps5vk_target_registers *);
/* Slice A measurement only: the target registers for one chosen array layer of
 * a surface, built from the same registers the single-layer path produces with
 * the base address advanced by that layer's footprint. It claims nothing: it
 * exists so the host test and the native probe can ask whether the pinned
 * GFX1013 AGC/DCB path can be pointed at a chosen layer at all, and whether
 * color and depth need different routing. `layer` is relative to the view's
 * baseArrayLayer, whose layerCount stays one. */
VkResult ps5vk_native_layer_target(VkDevice, VkImageView, uint32_t layer,
    const ps5_agc_register color_defaults[PS5_COLOR_REGISTER_COUNT], struct ps5vk_target_registers *);
/* Per-layer footprint of an attachment surface: the size one array layer
 * occupies under the same requirements/layout arithmetic the image path uses.
 * A layer stride must be a multiple of this for a layer-addressed target to be
 * expressible at all. */
VkResult ps5vk_native_layer_footprint(VkDevice, VkImage, VkDeviceSize *out);
#endif
