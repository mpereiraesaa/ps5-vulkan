/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../dxvk_render_witness/zero_initialize_compute.h"
#include "zero_initialize_fixture.h"

static int witness(void)
{
    VkResult result = VK_SUCCESS;
    const char *step = "initialization";
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkBool32 pending = VK_FALSE;
#define TRY(call) do { step = #call; result = (call); if (result != VK_SUCCESS) goto cleanup; } while (0)
#define REQUIRE(ok) do { step = #ok; if (!(ok)) { result = VK_ERROR_FEATURE_NOT_PRESENT; goto cleanup; } } while (0)
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app};
    TRY(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &count, &physical));
    REQUIRE(count == 1 && physical);
    VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &feature};
    vkGetPhysicalDeviceFeatures2(physical, &f2);
    REQUIRE(feature.shaderZeroInitializeWorkgroupMemory);
    feature.shaderZeroInitializeWorkgroupMemory = VK_TRUE;
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &feature, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    TRY(vkCreateDevice(physical, &dci, NULL, &device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue);
    ps5log_printf(PS5LOG_MARK, "ZERO_INITIALIZE_START groups=2 local=64 outputs=128 initialized_bytes=2048");
    struct zero_initialize_result observed;
    result = zero_initialize_compute_witness(device, queue, zero_initialize_spirv,
        sizeof(zero_initialize_spirv), &pending, &observed);
    step = observed.step;
    if (!pending && observed.outputs)
        ps5log_printf(PS5LOG_MARK,
            "ZERO_INITIALIZE_RESULT outputs=%u mismatches=%u guards=%u digest=%08x fence=complete",
            observed.outputs, observed.mismatches, observed.guards, observed.digest);
cleanup:
    if (result != VK_SUCCESS)
        ps5log_printf(PS5LOG_ERR, "ZERO_INITIALIZE_FAILURE result=%d step=%s",
            (int)result, step ? step : "unknown");
    if (pending) {
        ps5log_printf(PS5LOG_ERR, "ZERO_INITIALIZE_PENDING resources=retained");
        return 1;
    }
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK, "ZERO_INITIALIZE_RETIRED resources=clean");
    return result == VK_SUCCESS ? 0 : 1;
#undef TRY
#undef REQUIRE
}

int main(void)
{
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t boot = (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
    ps5log_config config;
    const char *loaded = NULL, *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&config);
    if (ps5log_load_config(paths, 1, &config, &loaded)) _exit(1);
    config.udp = 0;
    if (ps5log_init(&config, "PPSA99994", "ps5vk", boot)) _exit(1);
    int failed = witness();
    ps5log_close(failed ? "zero-initialize-failed" : "zero-initialize-end");
    for (;;) sleep(1);
}
