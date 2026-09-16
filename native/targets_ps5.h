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

/* At most one view per bit of a view mask: the structural width of a mask, not
 * a reported maxMultiviewViewCount, which this profile still does not claim. */
enum { PS5VK_MAX_VIEW_MASK_VIEWS = 32 };

/* Expand a subpass view mask into the views a prepared draw has to be
 * re-emitted for, in ascending order. `view_mask == 0` is exactly one view with
 * index zero - multiview disabled - so a pass that never asked for multiview
 * keeps the single draw and target it has always emitted.
 *
 * Fail-closed and atomic: more views than the caller can hold is refused, and
 * the caller's array is written only once every view is known, so a partial set
 * can never escape into an emission. */
VkResult ps5vk_native_view_expand(uint32_t view_mask, uint32_t *view_indices,
    uint32_t capacity, uint32_t *view_count);
VkResult ps5vk_native_target(VkDevice, VkImageView,
    const ps5_agc_register color_defaults[PS5_COLOR_REGISTER_COUNT], struct ps5vk_target_registers *);
/* Slice A measurement only: the target registers for one chosen array layer of
 * a surface, built from the same registers the single-layer path produces with
 * the base address advanced by that layer's footprint. It claims nothing: it
 * exists so the host test and the native probe can ask whether the pinned
 * GFX1013 AGC/DCB path can be pointed at a chosen layer at all, and whether
 * color and depth need different routing. `layer` is relative to the view's
 * baseArrayLayer and is bounded by the backing, not by the view's own range:
 * that is the measurement. A multiview caller must go through
 * ps5vk_native_view_layer_target, which adds the view's range obligation. */
VkResult ps5vk_native_layer_target(VkDevice, VkImageView, uint32_t layer,
    const ps5_agc_register color_defaults[PS5_COLOR_REGISTER_COUNT], struct ps5vk_target_registers *);
/* The target registers for one view of a layered attachment. The view's own
 * range has to cover the view - the multiview obligation that an attachment
 * view carries every view its subpass renders - and the shared per-layer
 * arithmetic then selects baseArrayLayer + view inside the real backing, so a
 * view can never name a layer the image does not have. */
VkResult ps5vk_native_view_layer_target(VkDevice, VkImageView, uint32_t view_index,
    const ps5_agc_register color_defaults[PS5_COLOR_REGISTER_COUNT], struct ps5vk_target_registers *);
/* Per-layer footprint of an attachment surface: the size one array layer
 * occupies under the same requirements/layout arithmetic the image path uses.
 * A layer stride must be a multiple of this for a layer-addressed target to be
 * expressible at all. */
VkResult ps5vk_native_layer_footprint(VkDevice, VkImage, VkDeviceSize *out);
#endif
