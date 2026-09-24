/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exact consumer expectations, independent of driver-private capability tables.
 * Compiled unchanged by the native consumer and the host query regression.
 * This validates reporting consistency; it is not hardware execution evidence.
 */
#ifndef PS5VK_CONSUMER_PHYSICAL_DEVICE_CONTRACT_H
#define PS5VK_CONSUMER_PHYSICAL_DEVICE_CONTRACT_H

static uint32_t fnv1a32(const uint8_t *bytes, size_t count)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static void report_physical_device_contract(VkInstance instance,
                                            VkPhysicalDevice physical_device,
                                            const VkPhysicalDeviceProperties *props)
{
    uint32_t count = 0;
    REQUIRE(vkEnumeratePhysicalDevices(instance, &count, NULL) == VK_SUCCESS &&
            count == 1, "physical-device count query");

    VkPhysicalDevice sentinel = (VkPhysicalDevice)(uintptr_t)0x1234;
    VkPhysicalDevice devices[2] = {sentinel, sentinel};
    count = 0;
    REQUIRE(vkEnumeratePhysicalDevices(instance, &count, devices) == VK_INCOMPLETE &&
            count == 0 && devices[0] == sentinel && devices[1] == sentinel,
            "physical-device zero-capacity query");
    count = 2;
    REQUIRE(vkEnumeratePhysicalDevices(instance, &count, devices) == VK_SUCCESS &&
            count == 1 && devices[0] == physical_device && devices[1] == sentinel,
            "physical-device oversized-capacity query");

    VkPhysicalDeviceMemoryProperties memory;
    memset(&memory, 0xa5, sizeof(memory));
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
    REQUIRE(memory.memoryHeapCount == 1 && memory.memoryTypeCount == 1 &&
            memory.memoryHeaps[0].size == UINT64_C(268435456) &&
            memory.memoryHeaps[0].flags == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT &&
            memory.memoryTypes[0].heapIndex == 0 &&
            memory.memoryTypes[0].propertyFlags ==
                (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
            "exact native memory report");

    VkQueueFamilyProperties queues[2];
    memset(queues, 0xa5, sizeof(queues));
    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, NULL);
    REQUIRE(count == 1, "queue-family count query");
    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, queues);
    for (size_t byte = 0; byte < sizeof(queues); ++byte)
        REQUIRE(((const uint8_t *)queues)[byte] == 0xa5,
                "queue-family zero-capacity preservation");
    count = 2;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, queues);
    REQUIRE(count == 1 && queues[0].queueCount == 1 &&
            queues[0].queueFlags == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) &&
            queues[0].minImageTransferGranularity.width == 1 &&
            queues[0].minImageTransferGranularity.height == 1 &&
            queues[0].minImageTransferGranularity.depth == 1,
            "exact queue-family report");
    for (size_t byte = sizeof(queues[0]); byte < sizeof(queues); ++byte)
        REQUIRE(((const uint8_t *)queues)[byte] == 0xa5,
                "queue-family tail preservation");

    VkBaseOutStructure unknown = {
        .sType = VK_STRUCTURE_TYPE_MAX_ENUM,
        .pNext = NULL,
    };
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &unknown,
    };
    vkGetPhysicalDeviceProperties2KHR(physical_device, &properties2);
    REQUIRE(!memcmp(&properties2.properties, props, sizeof(*props)) &&
            unknown.sType == VK_STRUCTURE_TYPE_MAX_ENUM && !unknown.pNext,
            "properties2 and unknown pNext preservation");

    VkFormatProperties bgra, rgba, depth, texel, unsupported;
    vkGetPhysicalDeviceFormatProperties(physical_device,
        VK_FORMAT_B8G8R8A8_UNORM, &bgra);
    vkGetPhysicalDeviceFormatProperties(physical_device,
        VK_FORMAT_R8G8B8A8_UNORM, &rgba);
    vkGetPhysicalDeviceFormatProperties(physical_device,
        VK_FORMAT_D32_SFLOAT, &depth);
    vkGetPhysicalDeviceFormatProperties(physical_device,
        VK_FORMAT_R32_UINT, &texel);
    vkGetPhysicalDeviceFormatProperties(physical_device,
        VK_FORMAT_UNDEFINED, &unsupported);
    /* The BC sampling witness uses the diagnostic SDK that also exposes
     * RGBA8 destinations for BC blits. Keep the exact shipping query intact. */
    VkFormatFeatureFlags bc_blit_dst = 0;
#if defined(CONSUMER_BC_FILTER_WITNESS) && CONSUMER_BC_FILTER_WITNESS
    bc_blit_dst = VK_FORMAT_FEATURE_BLIT_DST_BIT;
#endif
    REQUIRE(!bgra.linearTilingFeatures &&
            bgra.bufferFeatures == VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT &&
            bgra.optimalTilingFeatures == VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT &&
            /* RGBA8 is the one format with a linear-tiling role: the pinned
             * upstream draw module's host-readback staging image, whose only
             * usage is a transfer destination. */
            rgba.linearTilingFeatures ==
                (VK_FORMAT_FEATURE_TRANSFER_DST_BIT | bc_blit_dst) &&
            /* RGBA8 also carries the witnessed uniform-texel-buffer role. */
            rgba.bufferFeatures == (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
                                    VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) &&
            rgba.optimalTilingFeatures ==
                (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                 VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                 /* DXVK262-T06: the blend role the upstream dual-source
                  * family gates every leaf on, witnessed on hardware. */
                 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | bc_blit_dst) &&
            !depth.linearTilingFeatures && !depth.bufferFeatures &&
            /* TRANSFER_DST is the whole-subresource vkCmdClearDepthStencilImage
             * this profile executes, and TRANSFER_SRC is the whole-surface
             * depth readback its SW_64K_Z_X pixel addressing now supports. */
            depth.optimalTilingFeatures ==
                (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) &&
            texel.bufferFeatures == (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
                                     VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) &&
            !texel.linearTilingFeatures &&
            texel.optimalTilingFeatures == (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                            VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
            !unsupported.linearTilingFeatures &&
            !unsupported.optimalTilingFeatures && !unsupported.bufferFeatures,
            "exact format-property matrix");

    VkImageFormatProperties image;
    REQUIRE(vkGetPhysicalDeviceImageFormatProperties(physical_device,
                VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                0, &image) == VK_SUCCESS &&
            image.maxExtent.width == 16383 && image.maxExtent.height == 16383 &&
            image.maxExtent.depth == 1 && image.maxMipLevels == 1 &&
            image.maxArrayLayers == 1 &&
            image.sampleCounts == VK_SAMPLE_COUNT_1_BIT &&
            image.maxResourceSize == UINT64_C(268435456),
            "exact supported image-format query");
    memset(&image, 0xa5, sizeof(image));
    REQUIRE(vkGetPhysicalDeviceImageFormatProperties(physical_device,
                VK_FORMAT_UNDEFINED, VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT,
                0, &image) == VK_ERROR_FORMAT_NOT_SUPPORTED,
            "unsupported image-format result");
    VkImageFormatProperties zero_image = {0};
    REQUIRE(!memcmp(&image, &zero_image, sizeof(image)),
            "unsupported image-format zero report");

    const VkPhysicalDeviceLimits *limits = &props->limits;
    REQUIRE(props->apiVersion == VK_API_VERSION_1_0 &&
            props->vendorID == 0x1002 && props->deviceID == 0 &&
            props->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
            limits->maxStorageBufferRange == UINT32_C(268435456) &&
            limits->maxUniformBufferRange == 65536 &&
            limits->maxTexelBufferElements == 65536 &&
            limits->maxPushConstantsSize == 256 &&
            limits->maxMemoryAllocationCount == 2048 &&
            limits->bufferImageGranularity == UINT64_C(131072) &&
            limits->minMemoryMapAlignment == 64 &&
            limits->minTexelBufferOffsetAlignment == 4 &&
            limits->minUniformBufferOffsetAlignment == 256 &&
            limits->minStorageBufferOffsetAlignment == 256 &&
            limits->maxDescriptorSetUniformBuffersDynamic == 8 &&
            limits->maxDescriptorSetStorageBuffersDynamic == 4 &&
            limits->nonCoherentAtomSize == 64 &&
            limits->maxComputeSharedMemorySize == 65536 &&
            limits->maxComputeWorkGroupInvocations == 1024,
            "exact derived physical-device limits");

    char canonical[512];
    int written = snprintf(canonical, sizeof(canonical),
        "api=%08x vendor=%04x device=%04x heap=%llu heap_flags=%08x "
        "type_flags=%08x queue_flags=%08x storage=%u uniform=%u texel=%u "
        "push=%u allocations=%u granularity=%llu map_align=%llu texel_align=%llu "
        "ubo_align=%llu ssbo_align=%llu atom=%llu shared=%u invocations=%u",
        props->apiVersion, props->vendorID, props->deviceID,
        (unsigned long long)memory.memoryHeaps[0].size,
        memory.memoryHeaps[0].flags, memory.memoryTypes[0].propertyFlags,
        queues[0].queueFlags, limits->maxStorageBufferRange,
        limits->maxUniformBufferRange, limits->maxTexelBufferElements,
        limits->maxPushConstantsSize, limits->maxMemoryAllocationCount,
        (unsigned long long)limits->bufferImageGranularity,
        (unsigned long long)limits->minMemoryMapAlignment,
        (unsigned long long)limits->minTexelBufferOffsetAlignment,
        (unsigned long long)limits->minUniformBufferOffsetAlignment,
        (unsigned long long)limits->minStorageBufferOffsetAlignment,
        (unsigned long long)limits->nonCoherentAtomSize,
        limits->maxComputeSharedMemorySize,
        limits->maxComputeWorkGroupInvocations);
    REQUIRE(written > 0 && (size_t)written < sizeof(canonical),
            "physical-device canonical report size");
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_CONSUMER_PHYSICAL_DEVICE %s hash=%08x",
        canonical, fnv1a32((const uint8_t *)canonical, (size_t)written));
    ps5log_line(PS5LOG_MARK,
        "PS5VK_CONSUMER_PHYSICAL_QUERIES devices=1 queues=1 two_call=1 "
        "tail_preserved=1 pnext_preserved=1 formats=5 image_supported=1 "
        "image_rejected=1");
}
#endif
