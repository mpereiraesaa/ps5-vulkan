#ifndef PS5VK_IMAGE_LAYOUT_STATE_H
#define PS5VK_IMAGE_LAYOUT_STATE_H
#include "vk_image.h"
#include "depth_stencil_layout.h"
/* Entries are (image, aspect) pairs: a colour or depth-only image uses one,
 * a combined depth/stencil image one per aspect it has been named with. */
enum { PS5VK_LAYOUT_IMAGES=64 };
struct ps5vk_layout_entry {
    VkImage image;
    /* Exactly one aspect bit. */
    VkImageAspectFlags aspect;
    VkImageLayout initial, current;
};
struct ps5vk_layout_state {
    unsigned count;
    struct ps5vk_layout_entry entries[PS5VK_LAYOUT_IMAGES];
};
/* A preparation-local transaction, never a record-time image mutation.
 *
 * The whole-image forms name every aspect the image's format has, which is
 * what every single-aspect caller and every combined DEPTH_STENCIL barrier
 * means. The aspect forms name a subset: DEPTH, STENCIL or both on a combined
 * image, and exactly the image's own aspect otherwise. A layout is read
 * through each aspect (ps5vk_layout_for_aspect), so oldLayout matching and
 * the layout a later operation requires follow the Vulkan per-aspect rule.
 *
 * Every call is all-or-nothing: each named aspect is validated (aspect mask,
 * layout meaning, current layout, capacity) before any entry changes, so a
 * refused transition leaves the transaction exactly as it was. */
VkResult ps5vk_layout_transition(struct ps5vk_layout_state *, VkImage, VkImageLayout, VkImageLayout);
VkResult ps5vk_layout_require(const struct ps5vk_layout_state *, VkImage, VkImageLayout);
VkResult ps5vk_layout_transition_aspects(struct ps5vk_layout_state *, VkImage,
    VkImageAspectFlags, VkImageLayout, VkImageLayout);
VkResult ps5vk_layout_require_aspects(const struct ps5vk_layout_state *, VkImage,
    VkImageAspectFlags, VkImageLayout);
/* The layout one aspect has in this transaction: the pending one when the
 * transaction names it, the committed one otherwise. */
VkResult ps5vk_layout_current(const struct ps5vk_layout_state *, VkImage,
    VkImageAspectFlags aspect, VkImageLayout *out);
/* Caller must first prove exact GPU completion. Checks all snapshots before
 * committing any entry; queue serialization prohibits intervening commits. */
VkResult ps5vk_layout_commit(struct ps5vk_layout_state *);
#endif
