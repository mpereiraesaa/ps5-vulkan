/* SPDX-License-Identifier: GPL-3.0-or-later
 * DXVK v2.6.2 capability probe.  This consumer intentionally includes only
 * the staged public Vulkan ABI plus its own generated profile facts.
 */
#ifndef PS5VK_DXVK_CAPABILITY_PROBE_H
#define PS5VK_DXVK_CAPABILITY_PROBE_H

#include <stdint.h>
#include <string.h>

#include "dxvk_v262_profile.h"

struct dxvk262_probe_counts {
    uint32_t total;
    uint32_t satisfied;
};

static void dxvk262_requirement(struct dxvk262_probe_counts *counts,
                                const char *id, uint64_t expected,
                                uint64_t observed)
{
    const int pass = observed >= expected;
    ++counts->total;
    counts->satisfied += (uint32_t)pass;
    ps5log_printf(PS5LOG_MARK,
        "DXVK262_REQUIREMENT id=%s expected=%llu observed=%llu status=%s",
        id, (unsigned long long)expected, (unsigned long long)observed,
        pass ? "satisfied" : "blocker");
}

static uint32_t dxvk262_extension_version(const VkExtensionProperties *items,
                                          uint32_t count, const char *name)
{
    for (uint32_t index = 0; index < count; ++index)
        if (!strcmp(items[index].extensionName, name))
            return items[index].specVersion;
    return 0;
}

static int run_dxvk262_capability_probe(void)
{
    const char *instance_extensions[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        /* Probe what is present without asking a known-1.0 implementation to
         * accept the target version during instance creation. */
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create, NULL, &instance) != VK_SUCCESS) {
        ps5log_line(PS5LOG_ERR,
            "DXVK262_PROBE_END valid=0 compatible=0 reason=create-instance");
        ps5log_close("dxvk262-probe-create-instance");
        return 0;
    }

    uint32_t physical_count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    if (vkEnumeratePhysicalDevices(instance, &physical_count, &physical) != VK_SUCCESS ||
        physical_count != 1 || !physical) {
        ps5log_line(PS5LOG_ERR,
            "DXVK262_PROBE_END valid=0 compatible=0 reason=enumerate-device");
        vkDestroyInstance(instance, NULL);
        ps5log_close("dxvk262-probe-enumerate-device");
        return 0;
    }

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_TESSELLATION_LIMITS generation=%u patch=%u control_input=%u "
        "control_output=%u patch_output=%u total_output=%u evaluation_input=%u evaluation_output=%u",
        properties.limits.maxTessellationGenerationLevel,
        properties.limits.maxTessellationPatchSize,
        properties.limits.maxTessellationControlPerVertexInputComponents,
        properties.limits.maxTessellationControlPerVertexOutputComponents,
        properties.limits.maxTessellationControlPerPatchOutputComponents,
        properties.limits.maxTessellationControlTotalOutputComponents,
        properties.limits.maxTessellationEvaluationInputComponents,
        properties.limits.maxTessellationEvaluationOutputComponents);

    VkPhysicalDeviceTransformFeedbackFeaturesEXT transform_feedback = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
    };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .pNext = &transform_feedback,
    };
    VkPhysicalDeviceVulkan13Features features13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &robustness2,
    };
    VkPhysicalDeviceVulkan12Features features12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &features13,
    };
    VkPhysicalDeviceVulkan11Features features11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &features12,
    };
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &features11,
    };
    vkGetPhysicalDeviceFeatures2KHR(physical, &features2);

    VkPhysicalDeviceVulkan13Properties properties13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
    };
    VkPhysicalDeviceVulkan12Properties properties12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
        .pNext = &properties13,
    };
    VkPhysicalDeviceVulkan11Properties properties11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
        .pNext = &properties12,
    };
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &properties11,
    };
    vkGetPhysicalDeviceProperties2KHR(physical, &properties2);

    uint32_t extension_count = 0;
    VkExtensionProperties extensions[64];
    memset(extensions, 0, sizeof(extensions));
    VkResult extension_result = vkEnumerateDeviceExtensionProperties(
        physical, NULL, &extension_count, NULL);
    const uint32_t reported_extensions = extension_count;
    if (extension_count > 64) extension_count = 64;
    if (extension_result == VK_SUCCESS && extension_count)
        extension_result = vkEnumerateDeviceExtensionProperties(
            physical, NULL, &extension_count, extensions);

    /* The profile names promoted core fields; the 1.0 runtime exposes their
     * identical multiview semantics through KHR, not Vulkan 1.2 aggregates.
     * Keep the actual route explicit. This does not satisfy the API-version
     * requirement or claim DXVK can create a 1.3 device today. */
    if (properties.apiVersion < VK_API_VERSION_1_2 && extension_result == VK_SUCCESS &&
        dxvk262_extension_version(extensions, extension_count, VK_KHR_MULTIVIEW_EXTENSION_NAME)) {
        VkPhysicalDeviceMultiviewFeatures multiview = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
        VkPhysicalDeviceFeatures2 query = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &multiview};
        VkPhysicalDeviceMultiviewProperties limits = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
        VkPhysicalDeviceProperties2 properties_query = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &limits};
        vkGetPhysicalDeviceFeatures2KHR(physical, &query);
        vkGetPhysicalDeviceProperties2KHR(physical, &properties_query);
        features11.multiview = multiview.multiview;
        properties11.maxMultiviewViewCount = limits.maxMultiviewViewCount;
        properties11.maxMultiviewInstanceIndex = limits.maxMultiviewInstanceIndex;
        ps5log_printf(PS5LOG_MARK,
            "DXVK262_MULTIVIEW_QUERY route=VK_KHR_multiview multiview=%u "
            "maxMultiviewViewCount=%u maxMultiviewInstanceIndex=%u",
            multiview.multiview, limits.maxMultiviewViewCount, limits.maxMultiviewInstanceIndex);
    }
    if (properties.apiVersion < VK_API_VERSION_1_2 && extension_result == VK_SUCCESS &&
        dxvk262_extension_version(extensions, extension_count,
            VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME)) {
        VkPhysicalDeviceUniformBufferStandardLayoutFeatures standard_ubo = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES};
        VkPhysicalDeviceFeatures2 query = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &standard_ubo};
        vkGetPhysicalDeviceFeatures2KHR(physical, &query);
        features12.uniformBufferStandardLayout = standard_ubo.uniformBufferStandardLayout;
        ps5log_printf(PS5LOG_MARK,
            "DXVK262_STANDARD_UBO_QUERY route=VK_KHR_uniform_buffer_standard_layout "
            "uniformBufferStandardLayout=%u", standard_ubo.uniformBufferStandardLayout);
    }
    if (properties.apiVersion < VK_API_VERSION_1_2 && extension_result == VK_SUCCESS &&
        dxvk262_extension_version(extensions, extension_count,
            VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME)) {
        VkPhysicalDeviceVulkanMemoryModelFeaturesKHR memory_model = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 query = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &memory_model};
        vkGetPhysicalDeviceFeatures2KHR(physical, &query);
        features12.vulkanMemoryModel = memory_model.vulkanMemoryModel;
        features12.vulkanMemoryModelDeviceScope = memory_model.vulkanMemoryModelDeviceScope;
        ps5log_printf(PS5LOG_MARK,
            "DXVK262_MEMORY_MODEL_QUERY route=VK_KHR_vulkan_memory_model "
            "vulkanMemoryModel=%u vulkanMemoryModelDeviceScope=%u",
            memory_model.vulkanMemoryModel, memory_model.vulkanMemoryModelDeviceScope);
    }
    if (properties.apiVersion < VK_API_VERSION_1_2 && extension_result == VK_SUCCESS &&
        dxvk262_extension_version(extensions, extension_count,
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
        VkPhysicalDeviceBufferDeviceAddressFeaturesKHR device_address = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 query = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &device_address};
        vkGetPhysicalDeviceFeatures2KHR(physical, &query);
        features12.bufferDeviceAddress = device_address.bufferDeviceAddress;
        ps5log_printf(PS5LOG_MARK,
            "DXVK262_BUFFER_DEVICE_ADDRESS_QUERY route=VK_KHR_buffer_device_address "
            "bufferDeviceAddress=%u", device_address.bufferDeviceAddress);
    }
    if (properties.apiVersion < VK_API_VERSION_1_2 && extension_result == VK_SUCCESS &&
        dxvk262_extension_version(extensions, extension_count,
            VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME)) {
        VkPhysicalDeviceHostQueryResetFeaturesEXT host_query_reset = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 query = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &host_query_reset};
        vkGetPhysicalDeviceFeatures2KHR(physical, &query);
        features12.hostQueryReset = host_query_reset.hostQueryReset;
        ps5log_printf(PS5LOG_MARK,
            "DXVK262_HOST_QUERY_RESET_QUERY route=VK_EXT_host_query_reset "
            "hostQueryReset=%u", host_query_reset.hostQueryReset);
    }

    ps5log_printf(PS5LOG_MARK,
        "DXVK262_PROBE_BEGIN schema=1 profile=%s target_api=%s device_api=%u.%u.%u",
        DXVK262_PROFILE_ID, DXVK262_PROFILE_API_VERSION,
        VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
        VK_VERSION_PATCH(properties.apiVersion));
    struct dxvk262_probe_counts counts = {0};
    dxvk262_requirement(&counts, "api-version:apiVersion",
        VK_MAKE_API_VERSION(0, 1, 3, 204), properties.apiVersion);

#define REPORT_EXTENSION(name, version) \
    dxvk262_requirement(&counts, "extension:" name, version, \
        dxvk262_extension_version(extensions, extension_count, name));
    DXVK262_REQUIRED_EXTENSIONS(REPORT_EXTENSION)
#undef REPORT_EXTENSION

#define REPORT_CORE_FEATURE(name, expected) \
    dxvk262_requirement(&counts, "feature:VkPhysicalDeviceFeatures:" #name, \
        expected, features2.features.name);
    DXVK262_FEATURES_VK_PHYSICAL_DEVICE_FEATURES(REPORT_CORE_FEATURE)
#undef REPORT_CORE_FEATURE

#define REPORT_ROBUSTNESS2(name, expected) \
    dxvk262_requirement(&counts, "feature:VkPhysicalDeviceRobustness2FeaturesEXT:" #name, \
        expected, robustness2.name);
    DXVK262_FEATURES_VK_PHYSICAL_DEVICE_ROBUSTNESS2_FEATURES_EXT(REPORT_ROBUSTNESS2)
#undef REPORT_ROBUSTNESS2
#define REPORT_TRANSFORM_FEEDBACK(name, expected) \
    dxvk262_requirement(&counts, "feature:VkPhysicalDeviceTransformFeedbackFeaturesEXT:" #name, \
        expected, transform_feedback.name);
    DXVK262_FEATURES_VK_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT(REPORT_TRANSFORM_FEEDBACK)
#undef REPORT_TRANSFORM_FEEDBACK
#define REPORT_FEATURE11(name, expected) \
    dxvk262_requirement(&counts, "feature:VkPhysicalDeviceVulkan11Features:" #name, \
        expected, features11.name);
    DXVK262_FEATURES_VK_PHYSICAL_DEVICE_VULKAN11_FEATURES(REPORT_FEATURE11)
#undef REPORT_FEATURE11
#define REPORT_FEATURE12(name, expected) \
    dxvk262_requirement(&counts, "feature:VkPhysicalDeviceVulkan12Features:" #name, \
        expected, features12.name);
    DXVK262_FEATURES_VK_PHYSICAL_DEVICE_VULKAN12_FEATURES(REPORT_FEATURE12)
#undef REPORT_FEATURE12
#define REPORT_FEATURE13(name, expected) \
    dxvk262_requirement(&counts, "feature:VkPhysicalDeviceVulkan13Features:" #name, \
        expected, features13.name);
    DXVK262_FEATURES_VK_PHYSICAL_DEVICE_VULKAN13_FEATURES(REPORT_FEATURE13)
#undef REPORT_FEATURE13

#define REPORT_PROPERTY11(name, expected) \
    dxvk262_requirement(&counts, "property:VkPhysicalDeviceVulkan11Properties:" #name, \
        expected, properties11.name);
    DXVK262_PROPERTIES_VK_PHYSICAL_DEVICE_VULKAN11_PROPERTIES(REPORT_PROPERTY11)
#undef REPORT_PROPERTY11
#define REPORT_PROPERTY12(name, expected) \
    dxvk262_requirement(&counts, "property:VkPhysicalDeviceVulkan12Properties:" #name, \
        expected, properties12.name);
    DXVK262_PROPERTIES_VK_PHYSICAL_DEVICE_VULKAN12_PROPERTIES(REPORT_PROPERTY12)
#undef REPORT_PROPERTY12
#define REPORT_PROPERTY13(name, expected) \
    dxvk262_requirement(&counts, "property:VkPhysicalDeviceVulkan13Properties:" #name, \
        expected, properties13.name);
    DXVK262_PROPERTIES_VK_PHYSICAL_DEVICE_VULKAN13_PROPERTIES(REPORT_PROPERTY13)
#undef REPORT_PROPERTY13

    const int valid = extension_result == VK_SUCCESS &&
                      counts.total == DXVK262_PROFILE_REQUIREMENT_COUNT;
    const int compatible = valid && counts.satisfied == counts.total;
    ps5log_printf(PS5LOG_MARK,
        "DXVK262_PROBE_END valid=%d compatible=%d total=%u satisfied=%u blockers=%u "
        "device_extensions=%u",
        valid, compatible, counts.total, counts.satisfied,
        counts.total - counts.satisfied, reported_extensions);
    vkDestroyInstance(instance, NULL);
    ps5log_close("dxvk262-capability-probe");
    return 0;
}

#endif
