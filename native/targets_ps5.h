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

/* The exact register shapes the pinned target builders produce, and the words a
 * view's layer may move. A view selects a different address of the SAME image,
 * so only the address words can differ between the target a draw was prepared
 * with and that view's target: ps5_color_build_target writes address>>8 into the
 * word at 0x318 and (address>>40)&0xff into the word at 0x390, and the D32 plan
 * writes those same two halves into 0x012/0x014 and 0x01a/0x01c. Every other
 * word is that builder reading the same image, so a view that would move one is
 * refused rather than emitted.
 *
 * The tables ARE the builders' own lists - ps5_color_target.c's color_offsets
 * and ps5_depth_target.c's D32 plan, the latter including its constant
 * DB_RENDER_CONTROL word at 0x200 - and tests/test_targets_ps5.c pins both the
 * lists and the carriers against targets the real builders produce, so a builder
 * that changes shape fails that test instead of quietly widening this rule. */
static const uint32_t ps5vk_color_target_offsets[PS5_COLOR_REGISTER_COUNT] = {
    0x318, 0x31b, 0x31c, 0x31d, 0x31e, 0x31f, 0x321, 0x323,
    0x324, 0x325, 0x390, 0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8,
};
static const uint32_t ps5vk_depth_target_offsets[PS5_DEPTH_REGISTER_COUNT] = {
    0x000, 0x01f, 0x002, 0x004, 0x005, 0x007, 0x011, 0x012,
    0x013, 0x014, 0x015, 0x2af, 0x2de, 0x092, 0x01a, 0x01c,
    0x01b, 0x01d, 0x01e, 0x003, 0x010, 0x200,
};
/* The target shape a register count names, or NULL. The two roles are told
 * apart by their builder's own count, so any other length is not a target this
 * profile can re-emit for a view. */
static inline const uint32_t *ps5vk_target_offsets(uint32_t count)
{
    if (count == PS5_COLOR_REGISTER_COUNT) return ps5vk_color_target_offsets;
    if (count == PS5_DEPTH_REGISTER_COUNT) return ps5vk_depth_target_offsets;
    return NULL;
}
/* May this word carry the layer address? Immutable words the pipeline state
 * later rewrites (0x200's DB_RENDER_CONTROL policy, clip/cull, scissor,
 * rasterization precision) are not carriers here even though 0x200 is part of
 * the D32 target shape. */
static inline int ps5vk_target_carrier(uint32_t offset)
{
    switch (offset) {
    case 0x318: case 0x390:                             /* colour */
    case 0x012: case 0x014: case 0x01a: case 0x01c:     /* depth  */
        return 1;
    }
    return 0;
}

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
