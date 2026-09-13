#ifndef PS5VK_BUFFER_TRANSFER_H
#define PS5VK_BUFFER_TRANSFER_H
#include "vk_command.h"

VkBool32 ps5vk_buffer_transfer_operation(enum ps5vk_operation_type type);
VkResult ps5vk_buffer_transfer_validate(VkDevice device,
    const struct ps5vk_operation *operation);
VkResult ps5vk_buffer_transfer_execute(VkDevice device,
    const struct ps5vk_operation *operation);
#endif
