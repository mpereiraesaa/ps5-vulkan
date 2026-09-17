#ifndef PS5VK_INDIRECT_H
#define PS5VK_INDIRECT_H

#include "vk_command.h"

VkBool32 ps5vk_indirect_operation(enum ps5vk_operation_type type);
VkBool32 ps5vk_indirect_compute_operation(enum ps5vk_operation_type type);
VkBool32 ps5vk_indirect_graphics_operation(enum ps5vk_operation_type type);
/* Bytes of one argument structure of an indirect operation, zero for any other
 * operation type. */
size_t ps5vk_indirect_argument_size(enum ps5vk_operation_type type);
/* The exact byte length a vkCmdDraw*Indirect call with `count` commands at
 * `stride` reads from its offset, following the pinned valid-usage rules: a
 * count of zero reads nothing, a count of one reads exactly one structure and
 * ignores the stride, and a larger count needs a stride that is a multiple of
 * four and at least one structure, reading stride * (count - 1) + structure
 * bytes. The arithmetic is checked; an illegal shape or an overflow returns
 * VK_FALSE and leaves *length untouched. */
VkBool32 ps5vk_indirect_argument_span(enum ps5vk_operation_type type, uint32_t count,
                                      uint32_t stride, VkDeviceSize *length);
/* Whole-record validation of a recorded indirect operation against live device
 * state: the argument buffer usage and the complete argument span, the
 * physical maxDrawIndirectCount, and - for more than one command - the
 * multiDrawIndirect feature ENABLED on this logical device. */
VkResult ps5vk_indirect_validate(VkDevice device,
                                 const struct ps5vk_operation *operation);
/* Resolve command `index` (0 <= index < indirect_count) exactly when the
 * deferred submission reaches the queue head. The recorded operation remains
 * immutable; *resolved is a caller-owned direct snapshot whose draw_index is
 * `index`, with exactly that command's bytes invalidated before they are read.
 * A non-zero firstInstance is accepted only when drawIndirectFirstInstance is
 * enabled on the device; otherwise the command fails closed. */
VkResult ps5vk_indirect_resolve_command(VkDevice device,
                                        const struct ps5vk_operation *recorded,
                                        uint32_t index,
                                        struct ps5vk_operation *resolved);
/* Single-command entry point kept for the one-command shapes: a dispatch, a
 * graphics command with count one, or the count-zero no-op draw (resolved as a
 * zero-count direct draw). A record with more than one command is refused
 * here; callers expanding a multi-draw iterate ps5vk_indirect_resolve_command. */
VkResult ps5vk_indirect_resolve(VkDevice device,
                                const struct ps5vk_operation *recorded,
                                struct ps5vk_operation *resolved);

#endif
