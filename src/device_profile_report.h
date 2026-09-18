#ifndef PS5VK_DEVICE_PROFILE_REPORT_H
#define PS5VK_DEVICE_PROFILE_REPORT_H

#include "physical_device_profile.h"
#include "graphics_limits.h"

/* Single source of truth for the device profiles this repository ships.
 *
 * The native platform (native/platform_ps5.c) and the host-side reporting dump
 * (tools/dump_device_reporting.c) both build their VkPhysicalDeviceProperties
 * through ps5vk_device_profile_init, so the spec -> reported matrix describes
 * the values the console really queries instead of a hand-copied table.
 *
 * Two independent build switches matter and must not be conflated:
 *   - graphics_objects: the build exposes the graphics object model, which
 *     sizes the implementation heap and the allocation granularity;
 *   - graphics_submit: the build additionally enables the native graphics
 *     queue, which is what advertises the graphics limits and format matrix.
 * These values are project budgets and executable-frontend bounds, never
 * measurements of the console's memory or of GFX1013 capability. */

#define PS5VK_PROFILE_VENDOR_ID 0x1002u
#define PS5VK_PROFILE_COMPUTE_NAME "ps5vk gfx1013 experimental compute profile"
#define PS5VK_PROFILE_GRAPHICS_NAME "ps5vk gfx1013 experimental graphics profile"
#define PS5VK_PROFILE_COMPUTE_HEAP_BYTES (UINT64_C(64) * 1024 * 1024)
#define PS5VK_PROFILE_GRAPHICS_HEAP_BYTES (UINT64_C(256) * 1024 * 1024)
#define PS5VK_PROFILE_COMPUTE_ALLOCATION_GRANULARITY 65536u
#define PS5VK_PROFILE_GRAPHICS_ALLOCATION_GRANULARITY 131072u

static inline VkDeviceSize ps5vk_device_profile_heap_bytes(int graphics_objects)
{
    return graphics_objects ? PS5VK_PROFILE_GRAPHICS_HEAP_BYTES
                            : PS5VK_PROFILE_COMPUTE_HEAP_BYTES;
}

static inline VkDeviceSize ps5vk_device_profile_allocation_granularity(int graphics_objects)
{
    return graphics_objects ? PS5VK_PROFILE_GRAPHICS_ALLOCATION_GRANULARITY
                            : PS5VK_PROFILE_COMPUTE_ALLOCATION_GRANULARITY;
}

/* supported_features is the platform's PS5VK_FEATURE_* mask: the limits a
 * reported feature obliges (today maxDrawIndirectCount) are derived from it
 * here, so the native platform and the host reporting dump cannot report a
 * feature and its limit from two different sources. */
static inline void ps5vk_device_profile_init(VkPhysicalDeviceProperties *properties,
    VkPhysicalDeviceMemoryProperties *memory, int graphics_objects, int graphics_submit,
    uint32_t supported_features)
{
    const struct ps5vk_physical_profile_info info = {
        .name = graphics_submit ? PS5VK_PROFILE_GRAPHICS_NAME : PS5VK_PROFILE_COMPUTE_NAME,
        .vendor_id = PS5VK_PROFILE_VENDOR_ID,
        .heap_size = ps5vk_device_profile_heap_bytes(graphics_objects),
        .allocation_granularity = ps5vk_device_profile_allocation_granularity(graphics_objects),
        .buffer_image_granularity = ps5vk_device_profile_allocation_granularity(graphics_objects),
        .host_coherent = VK_FALSE,
    };
    ps5vk_physical_profile_init(properties, memory, &info);
    if (graphics_submit) ps5vk_graphics_limits(&properties->limits);
    properties->limits.maxDrawIndirectCount =
        ps5vk_platform_max_draw_indirect_count(supported_features);
    properties->limits.maxViewports =
        ps5vk_platform_max_viewports(supported_features);
}

#endif
