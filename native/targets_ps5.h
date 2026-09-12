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
#endif
