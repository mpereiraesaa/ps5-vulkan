/* Public exposure of VK_KHR_timeline_semaphore on the Vulkan 1.0 profile:
 * extension enumeration, the Features2/Properties2 chains, device enablement,
 * device-level lookup, and the host path DXVK drives through the public
 * entry points. Only platform discovery is mocked. */
#include "vk_internal.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t platform_features, platform_features_t09;
static uint64_t fake_now;
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
static uint64_t clock_ns(void *ctx) { (void)ctx; return fake_now; }
static void pause_ns(void *ctx, uint64_t remaining) { (void)ctx; (void)remaining; fake_now += 1000; }
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
        .max_allocation = 65536, .queue_flags = VK_QUEUE_COMPUTE_BIT,
        .supported_features = platform_features,
        .supported_features_t09 = platform_features_t09,
        .progress = {NULL, NULL, clock_ns, pause_ns}};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU", .heap_size = 65536,
        .allocation_granularity = 1, .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

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
static int lists_extension(VkPhysicalDevice p, uint32_t *spec)
{
    uint32_t count = 0;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS);
    VkExtensionProperties properties[16];
    assert(count <= 16);
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(properties[n].extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
            if (spec) *spec = properties[n].specVersion;
            return 1;
        }
    return 0;
}
static void query(VkPhysicalDevice p, VkBool32 *feature, uint64_t *difference)
{
    VkPhysicalDeviceTimelineSemaphoreFeatures f = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .timelineSemaphore = 2};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &f};
    vkGetPhysicalDeviceFeatures2KHR(p, &features);
    VkPhysicalDeviceTimelineSemaphoreProperties t = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES,
        .maxTimelineSemaphoreValueDifference = 12345};
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &t};
    vkGetPhysicalDeviceProperties2KHR(p, &properties);
    *feature = f.timelineSemaphore;
    *difference = t.maxTimelineSemaphoreValueDifference;
}
static VkResult create_device(VkPhysicalDevice p, int extension, const void *chain,
                              VkDevice *out)
{
    const char *name = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = chain,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &q,
        .enabledExtensionCount = extension ? 1u : 0u, .ppEnabledExtensionNames = &name};
    *out = VK_NULL_HANDLE;
    return vkCreateDevice(p, &info, NULL, out);
}

static void unsupported_platform(void)
{
    platform_features = PS5VK_FEATURE_ROBUST_BUFFER_ACCESS;
    platform_features_t09 = 0;
    VkInstance i = make_instance(1);
    VkPhysicalDevice p = physical(i);
    VkBool32 feature; uint64_t difference;
    query(p, &feature, &difference);
    assert(feature == VK_FALSE && difference == 0);
    assert(!lists_extension(p, NULL));
    VkDevice d;
    assert(create_device(p, 1, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    VkPhysicalDeviceTimelineSemaphoreFeatures f = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .timelineSemaphore = VK_TRUE};
    assert(create_device(p, 0, &f, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    vkDestroyInstance(i, NULL);
}

static void supported_platform(void)
{
    platform_features = PS5VK_FEATURE_ROBUST_BUFFER_ACCESS;
    platform_features_t09 = PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE;
    /* Vulkan 1.0: the extension depends on the instance's Features2 route. */
    VkInstance plain = make_instance(0);
    VkDevice d;
    assert(create_device(physical(plain), 1, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(plain, NULL);

    VkInstance i = make_instance(1);
    VkPhysicalDevice p = physical(i);
    VkBool32 feature; uint64_t difference;
    query(p, &feature, &difference);
    uint32_t spec = 0;
    assert(feature == VK_TRUE && lists_extension(p, &spec) &&
           spec == VK_KHR_TIMELINE_SEMAPHORE_SPEC_VERSION);
    /* The algorithm's bound: the full uint64_t range, above the profile's
     * 2^31-1 requirement and the upstream test's 2^32-1 floor. */
    assert(difference == UINT64_MAX && difference >= UINT64_C(2147483647));
    /* Extension negotiation preserves the declared device profile version. */
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(p, &properties);
    assert(properties.apiVersion == PS5VK_DEVICE_API_VERSION);

    VkPhysicalDeviceTimelineSemaphoreFeatures f = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .timelineSemaphore = VK_TRUE};
    /* The feature needs the extension; malformed or repeated structures fail. */
    assert(create_device(p, 0, &f, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    f.timelineSemaphore = 2;
    assert(create_device(p, 1, &f, &d) == VK_ERROR_UNKNOWN && !d);
    f.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceTimelineSemaphoreFeatures again = f;
    f.pNext = &again;
    assert(create_device(p, 1, &f, &d) == VK_ERROR_UNKNOWN && !d);
    f.pNext = NULL;

    /* Extension without the feature: commands resolve, timeline objects
     * cannot be created, binary type info is still accepted. */
    assert(create_device(p, 1, NULL, &d) == VK_SUCCESS && d);
    assert(d->timeline_extension_enabled &&
           !(d->enabled_features_t09 & PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE));
    assert(vkGetDeviceProcAddr(d, "vkWaitSemaphoresKHR"));
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    VkSemaphore s = VK_NULL_HANDLE;
    assert(vkCreateSemaphore(d, &si, NULL, &s) == VK_ERROR_UNKNOWN && !s);
    type.semaphoreType = VK_SEMAPHORE_TYPE_BINARY;
    assert(vkCreateSemaphore(d, &si, NULL, &s) == VK_SUCCESS && s);
    vkDestroySemaphore(d, s, NULL);
    vkDestroyDevice(d, NULL);

    /* No extension: the KHR commands are not reachable and the type structure
     * is unknown. */
    assert(create_device(p, 0, NULL, &d) == VK_SUCCESS && d);
    assert(!vkGetDeviceProcAddr(d, "vkGetSemaphoreCounterValueKHR") &&
           !vkGetDeviceProcAddr(d, "vkWaitSemaphoresKHR") &&
           !vkGetDeviceProcAddr(d, "vkSignalSemaphoreKHR"));
    assert(vkCreateSemaphore(d, &si, NULL, &s) == VK_ERROR_UNKNOWN && !s);
    vkDestroyDevice(d, NULL);

    /* The route DXVK uses: extension + feature, KHR entry points by lookup,
     * a timeline semaphore, submission with VkTimelineSemaphoreSubmitInfo,
     * waits with zero and finite timeouts, and the counter query. Vulkan 1.0
     * has no core names for these commands. */
    assert(create_device(p, 1, &f, &d) == VK_SUCCESS && d);
    assert(d->enabled_features_t09 & PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE);
    PFN_vkGetSemaphoreCounterValueKHR get_value = (PFN_vkGetSemaphoreCounterValueKHR)
        vkGetDeviceProcAddr(d, "vkGetSemaphoreCounterValueKHR");
    PFN_vkWaitSemaphoresKHR wait = (PFN_vkWaitSemaphoresKHR)
        vkGetDeviceProcAddr(d, "vkWaitSemaphoresKHR");
    PFN_vkSignalSemaphoreKHR signal = (PFN_vkSignalSemaphoreKHR)
        vkGetDeviceProcAddr(d, "vkSignalSemaphoreKHR");
    assert(get_value && wait && signal);
    assert(!vkGetDeviceProcAddr(d, "vkGetSemaphoreCounterValue") &&
           !vkGetDeviceProcAddr(d, "vkWaitSemaphores") &&
           !vkGetDeviceProcAddr(d, "vkSignalSemaphore"));
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 1;
    assert(vkCreateSemaphore(d, &si, NULL, &s) == VK_SUCCESS && s);
    VkQueue queue;
    vkGetDeviceQueue(d, 0, 0, &queue);
    uint64_t signal_value = 2;
    VkTimelineSemaphoreSubmitInfo values = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &signal_value};
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &values,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s};
    assert(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    uint64_t value = 0;
    assert(get_value(d, s, &value) == VK_SUCCESS && value == 2);
    uint64_t target = 3;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &s, .pValues = &target};
    assert(wait(d, &wi, 0) == VK_TIMEOUT);
    fake_now = 0;
    assert(wait(d, &wi, 10000) == VK_TIMEOUT && fake_now >= 10000);
    VkSemaphoreSignalInfo host = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = s, .value = 3};
    assert(signal(d, &host) == VK_SUCCESS);
    assert(wait(d, &wi, 10000) == VK_SUCCESS);
    assert(vkDeviceWaitIdle(d) == VK_SUCCESS);
    vkDestroySemaphore(d, s, NULL);
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
}

int main(void)
{
    unsupported_platform();
    supported_platform();
    puts("Timeline semaphore exposure: pass (host platform mock)");
    return 0;
}
