#ifndef PS5VK_IMAGE_LAYOUT_STATE_H
#define PS5VK_IMAGE_LAYOUT_STATE_H
#include "vk_image.h"
enum { PS5VK_LAYOUT_IMAGES=64 };
struct ps5vk_layout_entry { VkImage image; VkImageLayout initial, current; };
struct ps5vk_layout_state {
    unsigned count;
    struct ps5vk_layout_entry entries[PS5VK_LAYOUT_IMAGES];
};
/* A preparation-local transaction, never a record-time image mutation. */
VkResult ps5vk_layout_transition(struct ps5vk_layout_state *, VkImage, VkImageLayout, VkImageLayout);
VkResult ps5vk_layout_require(const struct ps5vk_layout_state *, VkImage, VkImageLayout);
/* Caller must first prove exact GPU completion. Checks all snapshots before
 * committing any entry; queue serialization prohibits intervening commits. */
VkResult ps5vk_layout_commit(struct ps5vk_layout_state *);
#endif
