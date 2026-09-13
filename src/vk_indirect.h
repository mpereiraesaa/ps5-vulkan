#ifndef PS5VK_INDIRECT_H
#define PS5VK_INDIRECT_H

#include "vk_command.h"

VkBool32 ps5vk_indirect_operation(enum ps5vk_operation_type type);
VkBool32 ps5vk_indirect_compute_operation(enum ps5vk_operation_type type);
VkBool32 ps5vk_indirect_graphics_operation(enum ps5vk_operation_type type);
VkResult ps5vk_indirect_validate(VkDevice device,
                                 const struct ps5vk_operation *operation);
/* Resolve exactly when a deferred submission reaches the queue head. The
 * recorded operation remains immutable; the returned direct operation is a
 * caller-owned snapshot suitable for backend preparation. */
VkResult ps5vk_indirect_resolve(VkDevice device,
                                const struct ps5vk_operation *recorded,
                                struct ps5vk_operation *resolved);

#endif
