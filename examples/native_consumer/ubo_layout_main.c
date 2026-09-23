#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "ubo_standard_layout_shader.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expr) do { \
    VkResult result = (expr); \
    if (result != VK_SUCCESS) { \
        ps5log_printf(PS5LOG_ERR, "CHECK failed: %s -> %d at %s:%d", \
                      #expr, (int)result, __FILE__, __LINE__); \
        ps5log_close("ubo-check-failed"); \
        exit(1); \
    } \
} while (0)

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        ps5log_printf(PS5LOG_ERR, "REQUIRE failed: %s", message); \
        ps5log_close("ubo-require-failed"); \
        exit(1); \
    } \
} while (0)

#include "ubo_standard_layout_witness.h"

int main(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t boot = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
    ps5log_config config;
    const char *loaded = NULL, *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&config);
    if (ps5log_load_config(paths, 1, &config, &loaded)) _exit(1);
    config.udp = 0;
    if (ps5log_init(&config, "PPSA99994", "ps5vk-ubo", boot)) _exit(1);

    const char *instance_extension =
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &instance_extension,
    };
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical != VK_NULL_HANDLE,
            "one public physical device");

    VkPhysicalDeviceUniformBufferStandardLayoutFeatures standard = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &standard,
    };
    vkGetPhysicalDeviceFeatures2KHR(physical, &features);
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_QUERY supported=%u robust=%u",
        standard.uniformBufferStandardLayout,
        features.features.robustBufferAccess);
    REQUIRE(standard.uniformBufferStandardLayout == VK_TRUE,
            "uniformBufferStandardLayout must be reported by this diagnostic build");
    REQUIRE(features.features.robustBufferAccess == VK_TRUE,
            "robustBufferAccess must be reported");

    VkExtensionProperties extensions[8];
    count = 8;
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions));
    unsigned found = 0;
    for (uint32_t i = 0; i < count; ++i)
        found |= !strcmp(extensions[i].extensionName,
                         VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME);
    REQUIRE(found, "VK_KHR_uniform_buffer_standard_layout enumeration");

    const char *device_extensions[2] = {
        VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
        VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME,
    };
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = device_extensions,
    };
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue != VK_NULL_HANDLE, "compute queue");
    run_ubo_standard_layout_witness(device, queue);

    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    ps5log_line(PS5LOG_MARK, "PS5VK_CONSUMER_UBO_STANDARD_LAYOUT_SUCCESS");
    ps5log_close("ubo-standard-layout-complete");
    return 0;
}
