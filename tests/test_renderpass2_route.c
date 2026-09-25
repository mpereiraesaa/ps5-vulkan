/* The Vulkan 1.0 extension route to separateDepthStencilLayouts:
 * VK_KHR_maintenance2, VK_KHR_create_renderpass2 (which needs
 * VK_KHR_multiview and VK_KHR_maintenance2 in the pinned registry) and
 * VK_KHR_separate_depth_stencil_layouts (which needs
 * VK_KHR_get_physical_device_properties2 and VK_KHR_create_renderpass2).
 * Enumeration, the feature query and device creation must agree with those
 * dependencies and with the platform bits. Only platform discovery is
 * mocked. */
#include "vk_internal.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t platform_features, platform_features_t09;
static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = malloc(size); *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, cache, cache};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
        .max_allocation = 65536, .queue_flags = VK_QUEUE_COMPUTE_BIT,
        .supported_features = platform_features,
        .supported_features_t09 = platform_features_t09};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU", .heap_size = 65536,
        .allocation_granularity = 1, .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static const char *const MAINTENANCE2 = VK_KHR_MAINTENANCE_2_EXTENSION_NAME;
static const char *const RP2 = VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
static const char *const MV = VK_KHR_MULTIVIEW_EXTENSION_NAME;
static const char *const SDS = VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME;

static VkInstance make_instance(int features2)
{
    const char *extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = features2 ? 1u : 0u, .ppEnabledExtensionNames = &extension};
    VkInstance i = VK_NULL_HANDLE;
    assert(vkCreateInstance(&info, NULL, &i) == VK_SUCCESS);
    return i;
}
static VkPhysicalDevice physical(VkInstance i)
{
    uint32_t count = 1;
    VkPhysicalDevice p = VK_NULL_HANDLE;
    assert(vkEnumeratePhysicalDevices(i, &count, &p) == VK_SUCCESS && p);
    return p;
}
static int lists(VkPhysicalDevice p, const char *name)
{
    uint32_t count = 0;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS);
    VkExtensionProperties properties[16];
    assert(count <= 16);
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(properties[n].extensionName, name)) {
            assert(properties[n].specVersion == 1);
            return 1;
        }
    return 0;
}
static VkBool32 feature(VkPhysicalDevice p)
{
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures f = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES,
        .separateDepthStencilLayouts = 2};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &f};
    vkGetPhysicalDeviceFeatures2KHR(p, &features);
    return f.separateDepthStencilLayouts;
}
static VkResult create(VkPhysicalDevice p, const char *const *names, uint32_t count,
                       VkBool32 separate, VkDevice *out)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures f = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES,
        .separateDepthStencilLayouts = separate};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = separate ? &f : NULL, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q,
        .enabledExtensionCount = count, .ppEnabledExtensionNames = names};
    *out = VK_NULL_HANDLE;
    return vkCreateDevice(p, &info, NULL, out);
}

/* No bits: nothing of the route is visible or accepted. */
static void closed(void)
{
    platform_features = PS5VK_FEATURE_MULTIVIEW;
    platform_features_t09 = PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS;
    VkInstance i = make_instance(1);
    VkPhysicalDevice p = physical(i);
    assert(!lists(p, MAINTENANCE2) && !lists(p, RP2) && !lists(p, SDS) && !feature(p));
    VkDevice d;
    const char *const all[4] = {MV, MAINTENANCE2, RP2, SDS};
    assert(create(p, all + 1, 1, VK_FALSE, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    assert(create(p, all, 3, VK_FALSE, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    assert(create(p, all, 4, VK_TRUE, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);
}

/* maintenance2 alone, or create_renderpass2 without multiview, does not open
 * the dependent extensions. */
static void partial(void)
{
    platform_features = PS5VK_FEATURE_MULTIVIEW;
    platform_features_t09 = PS5VK_T09_FEATURE_MAINTENANCE2 |
        PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS;
    VkInstance i = make_instance(1);
    VkPhysicalDevice p = physical(i);
    assert(lists(p, MAINTENANCE2) && !lists(p, RP2) && !lists(p, SDS) && !feature(p));
    VkDevice d;
    assert(create(p, &MAINTENANCE2, 1, VK_FALSE, &d) == VK_SUCCESS);
    assert(d->maintenance2_extension_enabled && !d->create_renderpass2_extension_enabled);
    assert(!vkGetDeviceProcAddr(d, "vkCreateRenderPass2KHR"));
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);

    platform_features = 0;
    platform_features_t09 = PS5VK_T09_FEATURE_MAINTENANCE2 |
        PS5VK_T09_FEATURE_CREATE_RENDERPASS2 | PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS;
    i = make_instance(1);
    p = physical(i);
    assert(lists(p, MAINTENANCE2) && !lists(p, RP2) && !lists(p, SDS) && !feature(p));
    const char *const names[2] = {MAINTENANCE2, RP2};
    assert(create(p, names, 2, VK_FALSE, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);
}

static void open_route(void)
{
    platform_features = PS5VK_FEATURE_MULTIVIEW;
    platform_features_t09 = PS5VK_T09_FEATURE_MAINTENANCE2 |
        PS5VK_T09_FEATURE_CREATE_RENDERPASS2 | PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS;
    VkInstance i = make_instance(1);
    VkPhysicalDevice p = physical(i);
    assert(lists(p, MAINTENANCE2) && lists(p, RP2) && lists(p, SDS) && lists(p, MV));
    assert(feature(p) == VK_TRUE);
    VkDevice d;
    /* Every dependency must be enabled with its dependent. */
    const char *const no_multiview[2] = {MAINTENANCE2, RP2};
    assert(create(p, no_multiview, 2, VK_FALSE, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    const char *const no_maintenance2[2] = {MV, RP2};
    assert(create(p, no_maintenance2, 2, VK_FALSE, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    const char *const no_rp2[3] = {MV, MAINTENANCE2, SDS};
    assert(create(p, no_rp2, 3, VK_FALSE, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    /* The feature needs its own extension. */
    const char *const route[4] = {MV, MAINTENANCE2, RP2, SDS};
    assert(create(p, route, 3, VK_TRUE, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    /* The route with the feature left off. */
    assert(create(p, route, 4, VK_FALSE, &d) == VK_SUCCESS);
    assert(d->maintenance2_extension_enabled && d->create_renderpass2_extension_enabled);
    assert(!d->enabled_features_t09);
    assert(vkGetDeviceProcAddr(d, "vkCreateRenderPass2KHR") &&
           vkGetDeviceProcAddr(d, "vkCmdBeginRenderPass2KHR") &&
           vkGetDeviceProcAddr(d, "vkCmdNextSubpass2KHR") &&
           vkGetDeviceProcAddr(d, "vkCmdEndRenderPass2KHR"));
    vkDestroyDevice(d, NULL);
    /* And with it. */
    assert(create(p, route, 4, VK_TRUE, &d) == VK_SUCCESS);
    assert(d->enabled_features_t09 == PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS);
    vkDestroyDevice(d, NULL);
    /* A duplicate name is invalid. */
    const char *const twice[2] = {MAINTENANCE2, MAINTENANCE2};
    assert(create(p, twice, 2, VK_FALSE, &d) != VK_SUCCESS && !d);
    vkDestroyInstance(i, NULL);

    /* Without VK_KHR_get_physical_device_properties2 on the instance the
     * route is refused (the separate extension and multiview both need it). */
    i = make_instance(0);
    p = physical(i);
    assert(create(p, route, 4, VK_FALSE, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);
}

int main(void)
{
    closed();
    partial();
    open_route();
    puts("renderpass2 route: maintenance2, create_renderpass2 and separate layouts follow the registry dependencies");
    return 0;
}
