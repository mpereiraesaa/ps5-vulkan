#ifndef PS5VK_NATIVE_PRESENT_H
#define PS5VK_NATIVE_PRESENT_H
#include "vk_image.h"
#include "command_arena_ps5.h"
#include "ps5_videoout.h"

/* Native interop only, not VkSwapchainKHR. Initialize to zero before open.
 * Open pins both images. After present completes, the displayed slot remains
 * unwritable until another slot replaces it or close succeeds. */
struct ps5vk_native_present {
    VkDevice device;
    VkImage images[2];
    struct ps5vk_command_arena arena;
    struct ps5_videoout video;
    uint64_t token;
    int displayed;
    unsigned open;
};
VkResult ps5vk_native_present_open(struct ps5vk_native_present *,VkDevice,VkImage,VkImage);
VkResult ps5vk_native_present_frame(struct ps5vk_native_present *,unsigned,uint64_t,unsigned);
VkResult ps5vk_native_present_close(struct ps5vk_native_present *);
VkResult ps5vk_native_present_once(VkDevice,VkImage,VkImage,uint64_t,unsigned);
#endif
