#include "vk_internal.h"
#include <string.h>

/* Static-library entry-point lookup, not an ICD/loader ABI implementation.
 * Only implemented commands enter this table: missing commands return NULL. */
enum scope { GLOBAL, INSTANCE, DEVICE };
struct entry { const char *name; PFN_vkVoidFunction function; enum scope scope; };
#define ENTRY(name, level) {#name, (PFN_vkVoidFunction)name, level}
static const struct entry entries[] = {
    ENTRY(vkGetInstanceProcAddr, GLOBAL),
    ENTRY(vkCreateInstance, GLOBAL),
    ENTRY(vkEnumerateInstanceExtensionProperties, GLOBAL),
    ENTRY(vkEnumerateInstanceLayerProperties, GLOBAL),
    ENTRY(vkEnumerateInstanceVersion, GLOBAL),
    ENTRY(vkDestroyInstance, INSTANCE),
    ENTRY(vkEnumeratePhysicalDevices, INSTANCE),
    ENTRY(vkGetPhysicalDeviceDisplayPropertiesKHR, INSTANCE),
    ENTRY(vkGetDisplayModePropertiesKHR, INSTANCE),
    ENTRY(vkCreateDisplayModeKHR, INSTANCE),
    ENTRY(vkGetPhysicalDeviceDisplayPlanePropertiesKHR, INSTANCE),
    ENTRY(vkGetDisplayPlaneSupportedDisplaysKHR, INSTANCE),
    ENTRY(vkGetDisplayPlaneCapabilitiesKHR, INSTANCE),
    ENTRY(vkCreateDisplayPlaneSurfaceKHR, INSTANCE),
    ENTRY(vkDestroySurfaceKHR, INSTANCE),
    ENTRY(vkGetPhysicalDeviceSurfaceSupportKHR, INSTANCE),
    ENTRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, INSTANCE),
    ENTRY(vkGetPhysicalDeviceSurfaceFormatsKHR, INSTANCE),
    ENTRY(vkGetPhysicalDeviceSurfacePresentModesKHR, INSTANCE),
    ENTRY(vkEnumeratePhysicalDeviceGroupsKHR, INSTANCE),
    ENTRY(vkEnumeratePhysicalDeviceGroups, INSTANCE),
    ENTRY(vkGetPhysicalDeviceFeatures2, INSTANCE),
    ENTRY(vkGetPhysicalDeviceProperties2, INSTANCE),
    ENTRY(vkGetPhysicalDeviceFormatProperties2, INSTANCE),
    ENTRY(vkGetPhysicalDeviceImageFormatProperties2, INSTANCE),
    ENTRY(vkGetPhysicalDeviceQueueFamilyProperties2, INSTANCE),
    ENTRY(vkGetPhysicalDeviceMemoryProperties2, INSTANCE),
    ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties2, INSTANCE),
    ENTRY(vkGetPhysicalDeviceExternalBufferProperties, INSTANCE),
    ENTRY(vkGetPhysicalDeviceExternalFenceProperties, INSTANCE),
    ENTRY(vkGetPhysicalDeviceExternalSemaphoreProperties, INSTANCE),
    ENTRY(vkGetPhysicalDeviceProperties, INSTANCE),
    ENTRY(vkGetPhysicalDeviceMemoryProperties, INSTANCE),
    ENTRY(vkGetPhysicalDeviceFeatures, INSTANCE),
    ENTRY(vkGetPhysicalDeviceFeatures2KHR, INSTANCE),
    ENTRY(vkGetPhysicalDeviceProperties2KHR, INSTANCE),
    ENTRY(vkGetPhysicalDeviceMemoryProperties2KHR, INSTANCE),
    ENTRY(vkGetPhysicalDeviceFormatProperties, INSTANCE),
    ENTRY(vkGetPhysicalDeviceFormatProperties2KHR, INSTANCE),
    ENTRY(vkGetPhysicalDeviceImageFormatProperties, INSTANCE),
    ENTRY(vkGetPhysicalDeviceImageFormatProperties2KHR, INSTANCE),
    ENTRY(vkGetPhysicalDeviceQueueFamilyProperties, INSTANCE),
    ENTRY(vkGetPhysicalDeviceQueueFamilyProperties2KHR, INSTANCE),
    ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties2KHR, INSTANCE),
    ENTRY(vkEnumerateDeviceExtensionProperties, INSTANCE),
    ENTRY(vkEnumerateDeviceLayerProperties, INSTANCE),
    ENTRY(vkCreateDevice, INSTANCE),
    ENTRY(vkGetDeviceProcAddr, DEVICE),
    ENTRY(vkDestroyDevice, DEVICE),
    ENTRY(vkGetDeviceQueue, DEVICE),
    ENTRY(vkCreateSwapchainKHR, DEVICE),
    ENTRY(vkDestroySwapchainKHR, DEVICE),
    ENTRY(vkGetSwapchainImagesKHR, DEVICE),
    ENTRY(vkAcquireNextImageKHR, DEVICE),
    ENTRY(vkQueuePresentKHR, DEVICE),
    ENTRY(vkGetDeviceGroupPeerMemoryFeaturesKHR, DEVICE),
    ENTRY(vkAllocateMemory, DEVICE),
    ENTRY(vkFreeMemory, DEVICE),
    ENTRY(vkMapMemory, DEVICE),
    ENTRY(vkUnmapMemory, DEVICE),
    ENTRY(vkFlushMappedMemoryRanges, DEVICE),
    ENTRY(vkInvalidateMappedMemoryRanges, DEVICE),
    ENTRY(vkGetDeviceMemoryCommitment, DEVICE),
    ENTRY(vkCreateBuffer, DEVICE),
    ENTRY(vkDestroyBuffer, DEVICE),
    ENTRY(vkCreateBufferView, DEVICE),
    ENTRY(vkDestroyBufferView, DEVICE),
    ENTRY(vkGetBufferMemoryRequirements, DEVICE),
    ENTRY(vkBindBufferMemory, DEVICE),
    ENTRY(vkGetBufferDeviceAddressKHR, DEVICE),
    ENTRY(vkGetBufferOpaqueCaptureAddressKHR, DEVICE),
    ENTRY(vkGetDeviceMemoryOpaqueCaptureAddressKHR, DEVICE),
    ENTRY(vkCreateImage, DEVICE),
    ENTRY(vkDestroyImage, DEVICE),
    ENTRY(vkGetImageMemoryRequirements, DEVICE),
    ENTRY(vkGetImageSubresourceLayout, DEVICE),
    ENTRY(vkBindImageMemory, DEVICE),
    ENTRY(vkGetBufferMemoryRequirements2KHR, DEVICE),
    ENTRY(vkGetImageMemoryRequirements2KHR, DEVICE),
    ENTRY(vkGetImageSparseMemoryRequirements2KHR, DEVICE),
    ENTRY(vkBindBufferMemory2KHR, DEVICE),
    ENTRY(vkBindImageMemory2KHR, DEVICE),
    ENTRY(vkGetDeviceBufferMemoryRequirementsKHR, DEVICE),
    ENTRY(vkGetDeviceImageMemoryRequirementsKHR, DEVICE),
    ENTRY(vkGetDeviceImageSparseMemoryRequirementsKHR, DEVICE),
    ENTRY(vkCreateImageView, DEVICE),
    ENTRY(vkDestroyImageView, DEVICE),
    ENTRY(vkCreateSampler, DEVICE),
    ENTRY(vkDestroySampler, DEVICE),
    ENTRY(vkCreateRenderPass, DEVICE),
    ENTRY(vkDestroyRenderPass, DEVICE),
    ENTRY(vkGetRenderAreaGranularity, DEVICE),
    ENTRY(vkCreateRenderPass2KHR, DEVICE),
    ENTRY(vkCreateFramebuffer, DEVICE),
    ENTRY(vkDestroyFramebuffer, DEVICE),
    ENTRY(vkCreateDescriptorSetLayout, DEVICE),
    ENTRY(vkDestroyDescriptorSetLayout, DEVICE),
    ENTRY(vkCreateDescriptorPool, DEVICE),
    ENTRY(vkDestroyDescriptorPool, DEVICE),
    ENTRY(vkResetDescriptorPool, DEVICE),
    ENTRY(vkAllocateDescriptorSets, DEVICE),
    ENTRY(vkFreeDescriptorSets, DEVICE),
    ENTRY(vkUpdateDescriptorSets, DEVICE),
    ENTRY(vkCreatePipelineLayout, DEVICE),
    ENTRY(vkDestroyPipelineLayout, DEVICE),
    ENTRY(vkCreateShaderModule, DEVICE),
    ENTRY(vkDestroyShaderModule, DEVICE),
    ENTRY(vkCreateComputePipelines, DEVICE),
    ENTRY(vkCreateGraphicsPipelines, DEVICE),
    ENTRY(vkDestroyPipeline, DEVICE),
    ENTRY(vkCreateCommandPool, DEVICE),
    ENTRY(vkDestroyCommandPool, DEVICE),
    ENTRY(vkResetCommandPool, DEVICE),
    ENTRY(vkAllocateCommandBuffers, DEVICE),
    ENTRY(vkFreeCommandBuffers, DEVICE),
    ENTRY(vkBeginCommandBuffer, DEVICE),
    ENTRY(vkEndCommandBuffer, DEVICE),
    ENTRY(vkResetCommandBuffer, DEVICE),
    ENTRY(vkCmdBindPipeline, DEVICE),
    ENTRY(vkCmdSetViewport, DEVICE),
    ENTRY(vkCmdSetScissor, DEVICE),
    ENTRY(vkCmdSetLineWidth, DEVICE),
    ENTRY(vkCmdSetDepthBias, DEVICE),
    ENTRY(vkCmdSetBlendConstants, DEVICE),
    ENTRY(vkCmdSetDepthBounds, DEVICE),
    ENTRY(vkCmdSetStencilCompareMask, DEVICE),
    ENTRY(vkCmdSetStencilWriteMask, DEVICE),
    ENTRY(vkCmdSetStencilReference, DEVICE),
    ENTRY(vkCmdBindDescriptorSets, DEVICE),
    ENTRY(vkCmdPushConstants, DEVICE),
    ENTRY(vkCmdDispatch, DEVICE),
    ENTRY(vkCmdDispatchBaseKHR, DEVICE),
    ENTRY(vkCmdSetDeviceMaskKHR, DEVICE),
    ENTRY(vkCmdDispatchIndirect, DEVICE),
    ENTRY(vkCmdBeginRenderPass, DEVICE),
    ENTRY(vkCmdNextSubpass, DEVICE),
    ENTRY(vkCmdEndRenderPass, DEVICE),
    ENTRY(vkCmdBeginRenderPass2KHR, DEVICE),
    ENTRY(vkCmdNextSubpass2KHR, DEVICE),
    ENTRY(vkCmdEndRenderPass2KHR, DEVICE),
    ENTRY(vkCmdExecuteCommands, DEVICE),
    ENTRY(vkCmdBindVertexBuffers, DEVICE),
    ENTRY(vkCmdBindIndexBuffer, DEVICE),
    ENTRY(vkCmdDraw, DEVICE),
    ENTRY(vkCmdDrawIndexed, DEVICE),
    ENTRY(vkCmdDrawIndirect, DEVICE),
    ENTRY(vkCmdDrawIndexedIndirect, DEVICE),
    ENTRY(vkCmdCopyBuffer, DEVICE),
    ENTRY(vkCmdUpdateBuffer, DEVICE),
    ENTRY(vkCmdFillBuffer, DEVICE),
    ENTRY(vkCmdCopyImage, DEVICE),
    ENTRY(vkCmdBlitImage, DEVICE),
    ENTRY(vkCmdResolveImage, DEVICE),
    ENTRY(vkCmdClearColorImage, DEVICE),
    ENTRY(vkCmdClearDepthStencilImage, DEVICE),
    ENTRY(vkCmdClearAttachments, DEVICE),
    ENTRY(vkCmdCopyBufferToImage, DEVICE),
    ENTRY(vkCmdCopyImageToBuffer, DEVICE),
    ENTRY(vkCmdPipelineBarrier, DEVICE),
    ENTRY(vkCreateFence, DEVICE),
    ENTRY(vkDestroyFence, DEVICE),
    ENTRY(vkResetFences, DEVICE),
    ENTRY(vkGetFenceStatus, DEVICE),
    ENTRY(vkWaitForFences, DEVICE),
    ENTRY(vkCreateSemaphore, DEVICE),
    ENTRY(vkDestroySemaphore, DEVICE),
    ENTRY(vkGetSemaphoreCounterValueKHR, DEVICE),
    ENTRY(vkWaitSemaphoresKHR, DEVICE),
    ENTRY(vkSignalSemaphoreKHR, DEVICE),
    ENTRY(vkCreateDescriptorUpdateTemplateKHR, DEVICE),
    ENTRY(vkDestroyDescriptorUpdateTemplateKHR, DEVICE),
    ENTRY(vkUpdateDescriptorSetWithTemplateKHR, DEVICE),
    ENTRY(vkCreateEvent, DEVICE),
    ENTRY(vkDestroyEvent, DEVICE),
    ENTRY(vkGetEventStatus, DEVICE),
    ENTRY(vkSetEvent, DEVICE),
    ENTRY(vkResetEvent, DEVICE),
    ENTRY(vkCmdSetEvent, DEVICE),
    ENTRY(vkCmdResetEvent, DEVICE),
    ENTRY(vkCmdWaitEvents, DEVICE),
    ENTRY(vkQueueSubmit, DEVICE),
    ENTRY(vkQueueBindSparse, DEVICE),
    ENTRY(vkQueueWaitIdle, DEVICE),
    ENTRY(vkDeviceWaitIdle, DEVICE),
    ENTRY(vkCreatePipelineCache, DEVICE),
    ENTRY(vkDestroyPipelineCache, DEVICE),
    ENTRY(vkGetPipelineCacheData, DEVICE),
    ENTRY(vkMergePipelineCaches, DEVICE),
    ENTRY(vkCreateQueryPool, DEVICE),
    ENTRY(vkDestroyQueryPool, DEVICE),
    ENTRY(vkGetQueryPoolResults, DEVICE),
    ENTRY(vkResetQueryPool, DEVICE),
    ENTRY(vkResetQueryPoolEXT, DEVICE),
    ENTRY(vkCmdResetQueryPool, DEVICE),
    ENTRY(vkCmdBeginQuery, DEVICE),
    ENTRY(vkCmdEndQuery, DEVICE),
    ENTRY(vkCmdWriteTimestamp, DEVICE),
    ENTRY(vkCmdCopyQueryPoolResults, DEVICE),
    ENTRY(vkGetImageSparseMemoryRequirements, DEVICE),
    ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties, INSTANCE),
    /* VK_EXT_extended_dynamic_state (DXVK262-T10). */
    ENTRY(vkCmdSetCullModeEXT, DEVICE),
    ENTRY(vkCmdSetFrontFaceEXT, DEVICE),
    ENTRY(vkCmdSetPrimitiveTopologyEXT, DEVICE),
    ENTRY(vkCmdSetViewportWithCountEXT, DEVICE),
    ENTRY(vkCmdSetScissorWithCountEXT, DEVICE),
    ENTRY(vkCmdBindVertexBuffers2EXT, DEVICE),
    ENTRY(vkCmdSetDepthTestEnableEXT, DEVICE),
    ENTRY(vkCmdSetDepthWriteEnableEXT, DEVICE),
    ENTRY(vkCmdSetDepthCompareOpEXT, DEVICE),
    ENTRY(vkCmdSetDepthBoundsTestEnableEXT, DEVICE),
    ENTRY(vkCmdSetStencilTestEnableEXT, DEVICE),
    ENTRY(vkCmdSetStencilOpEXT, DEVICE),
    /* VK_KHR_maintenance1 (DXVK262-T10). */
    ENTRY(vkTrimCommandPoolKHR, DEVICE),
    /* VK_KHR_copy_commands2 (DXVK262-T10). */
    ENTRY(vkCmdCopyBuffer2KHR, DEVICE),
    ENTRY(vkCmdCopyImage2KHR, DEVICE),
    ENTRY(vkCmdCopyBufferToImage2KHR, DEVICE),
    ENTRY(vkCmdCopyImageToBuffer2KHR, DEVICE),
    ENTRY(vkCmdBlitImage2KHR, DEVICE),
    ENTRY(vkCmdResolveImage2KHR, DEVICE),
    /* VK_KHR_dynamic_rendering (DXVK262-T10). */
    ENTRY(vkCmdBeginRenderingKHR, DEVICE),
    ENTRY(vkCmdEndRenderingKHR, DEVICE),
};
#undef ENTRY

static int gpdp2_command(const char *name)
{
    static const char *const commands[] = {
        "vkGetPhysicalDeviceFeatures2KHR",
        "vkGetPhysicalDeviceProperties2KHR",
        "vkGetPhysicalDeviceFormatProperties2KHR",
        "vkGetPhysicalDeviceImageFormatProperties2KHR",
        "vkGetPhysicalDeviceQueueFamilyProperties2KHR",
        "vkGetPhysicalDeviceMemoryProperties2KHR",
        "vkGetPhysicalDeviceSparseImageFormatProperties2KHR",
    };
    for (size_t n = 0; n < sizeof(commands) / sizeof(commands[0]); ++n)
        if (!strcmp(name, commands[n])) return 1;
    return 0;
}

static int core11_instance_command(const char *name)
{
    static const char *const commands[] = {
        "vkEnumeratePhysicalDeviceGroups",
        "vkGetPhysicalDeviceFeatures2",
        "vkGetPhysicalDeviceProperties2",
        "vkGetPhysicalDeviceFormatProperties2",
        "vkGetPhysicalDeviceImageFormatProperties2",
        "vkGetPhysicalDeviceQueueFamilyProperties2",
        "vkGetPhysicalDeviceMemoryProperties2",
        "vkGetPhysicalDeviceSparseImageFormatProperties2",
        "vkGetPhysicalDeviceExternalBufferProperties",
        "vkGetPhysicalDeviceExternalFenceProperties",
        "vkGetPhysicalDeviceExternalSemaphoreProperties",
    };
    for (size_t n = 0; n < sizeof(commands) / sizeof(commands[0]); ++n)
        if (!strcmp(name, commands[n])) return 1;
    return 0;
}

static int group_creation_command(const char *name)
{ return !strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR"); }

static int display_command(const char *name)
{
    return !strcmp(name, "vkGetPhysicalDeviceDisplayPropertiesKHR") ||
           !strcmp(name, "vkGetDisplayModePropertiesKHR") ||
           !strcmp(name, "vkCreateDisplayModeKHR") ||
           !strcmp(name, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR") ||
           !strcmp(name, "vkGetDisplayPlaneSupportedDisplaysKHR") ||
           !strcmp(name, "vkGetDisplayPlaneCapabilitiesKHR") ||
           !strcmp(name, "vkCreateDisplayPlaneSurfaceKHR");
}
static int surface_command(const char *name)
{
    return !strcmp(name, "vkDestroySurfaceKHR") ||
           !strcmp(name, "vkGetPhysicalDeviceSurfaceSupportKHR") ||
           !strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") ||
           !strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR") ||
           !strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR");
}

static int swapchain_command(const char *name)
{
    return !strcmp(name, "vkCreateSwapchainKHR") ||
           !strcmp(name, "vkDestroySwapchainKHR") ||
           !strcmp(name, "vkGetSwapchainImagesKHR") ||
           !strcmp(name, "vkAcquireNextImageKHR") ||
           !strcmp(name, "vkQueuePresentKHR");
}

static int device_group_command(const char *name)
{
    return !strcmp(name, "vkCmdDispatchBaseKHR") ||
           !strcmp(name, "vkCmdSetDeviceMaskKHR") ||
           !strcmp(name, "vkGetDeviceGroupPeerMemoryFeaturesKHR");
}

/* VK_KHR_create_renderpass2: visible only on a device that enabled it. This
 * device reports Vulkan 1.0, so the core-1.2 names without the suffix are not
 * entries at all. */
static int create_renderpass2_command(const char *name)
{
    return !strcmp(name, "vkCreateRenderPass2KHR") ||
           !strcmp(name, "vkCmdBeginRenderPass2KHR") ||
           !strcmp(name, "vkCmdNextSubpass2KHR") ||
           !strcmp(name, "vkCmdEndRenderPass2KHR");
}
static int buffer_device_address_command(const char *name)
{
    return !strcmp(name, "vkGetBufferDeviceAddressKHR") ||
           !strcmp(name, "vkGetBufferOpaqueCaptureAddressKHR") ||
           !strcmp(name, "vkGetDeviceMemoryOpaqueCaptureAddressKHR");
}

/* VK_KHR_get_memory_requirements2 and VK_KHR_bind_memory2, visible only on a
 * device that enabled them. The core-1.1 names without the suffix are not
 * entries: this device reports Vulkan 1.0. VK_KHR_dedicated_allocation adds
 * structures, no commands. */
static int memory_requirements2_command(const char *name)
{
    return !strcmp(name, "vkGetBufferMemoryRequirements2KHR") ||
           !strcmp(name, "vkGetImageMemoryRequirements2KHR") ||
           !strcmp(name, "vkGetImageSparseMemoryRequirements2KHR");
}
static int bind_memory2_command(const char *name)
{
    return !strcmp(name, "vkBindBufferMemory2KHR") ||
           !strcmp(name, "vkBindImageMemory2KHR");
}

/* VK_KHR_maintenance4 queries; the core-1.3 names are not entries. */
static int maintenance4_command(const char *name)
{
    return !strcmp(name, "vkGetDeviceBufferMemoryRequirementsKHR") ||
           !strcmp(name, "vkGetDeviceImageMemoryRequirementsKHR") ||
           !strcmp(name, "vkGetDeviceImageSparseMemoryRequirementsKHR");
}

/* VK_KHR_timeline_semaphore host commands, reachable only on a device that
 * enabled the extension. Vulkan 1.0 has no core names for them. */
static int descriptor_update_template_command(const char *name)
{
    /* VK_KHR_descriptor_update_template; the core-1.1 names arrive only with
     * an apiVersion promotion. Push-descriptor template commands belong to
     * VK_KHR_push_descriptor and are not implemented. */
    return !strcmp(name, "vkCreateDescriptorUpdateTemplateKHR") ||
           !strcmp(name, "vkDestroyDescriptorUpdateTemplateKHR") ||
           !strcmp(name, "vkUpdateDescriptorSetWithTemplateKHR");
}

static int timeline_semaphore_command(const char *name)
{
    return !strcmp(name, "vkGetSemaphoreCounterValueKHR") ||
           !strcmp(name, "vkWaitSemaphoresKHR") ||
           !strcmp(name, "vkSignalSemaphoreKHR");
}

/* VK_EXT_extended_dynamic_state commands, reachable only on a device that
 * enabled the extension. Vulkan 1.0 has no core names for them. */
static int extended_dynamic_state_command(const char *name)
{
    static const char *const commands[] = {
        "vkCmdSetCullModeEXT", "vkCmdSetFrontFaceEXT", "vkCmdSetPrimitiveTopologyEXT",
        "vkCmdSetViewportWithCountEXT", "vkCmdSetScissorWithCountEXT",
        "vkCmdBindVertexBuffers2EXT", "vkCmdSetDepthTestEnableEXT",
        "vkCmdSetDepthWriteEnableEXT", "vkCmdSetDepthCompareOpEXT",
        "vkCmdSetDepthBoundsTestEnableEXT", "vkCmdSetStencilTestEnableEXT",
        "vkCmdSetStencilOpEXT",
    };
    for (size_t n = 0; n < sizeof(commands) / sizeof(commands[0]); ++n)
        if (!strcmp(name, commands[n])) return 1;
    return 0;
}

/* VK_KHR_copy_commands2 commands, reachable only on a device that enabled
 * the extension. Vulkan 1.0 has no core names for them. */
static int copy_commands2_command(const char *name)
{
    return !strcmp(name, "vkCmdCopyBuffer2KHR") || !strcmp(name, "vkCmdCopyImage2KHR") ||
           !strcmp(name, "vkCmdCopyBufferToImage2KHR") ||
           !strcmp(name, "vkCmdCopyImageToBuffer2KHR") ||
           !strcmp(name, "vkCmdBlitImage2KHR") || !strcmp(name, "vkCmdResolveImage2KHR");
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                              const char *name)
{
    if (!name) return NULL;
    if (gpdp2_command(name) &&
        (!instance || !instance->features2_extension_enabled))
        return NULL;
    if (group_creation_command(name) &&
        (!instance || !instance->device_group_creation_enabled))
        return NULL;
    if (display_command(name) &&
        (!instance || !instance->display_extension_enabled))
        return NULL;
    if (surface_command(name) &&
        (!instance || !instance->surface_extension_enabled))
        return NULL;
    /* Core 1.1 instance- and physical-device-level names: only for an
     * instance created with apiVersion 1.1 or later. Device-level core 1.1
     * names stay absent because the device reports Vulkan 1.0. */
    if (core11_instance_command(name) &&
        (!instance || instance->api_version < VK_API_VERSION_1_1))
        return NULL;
    for (size_t j = 0; j < sizeof(entries) / sizeof(entries[0]); ++j)
        if ((instance || entries[j].scope == GLOBAL) && !strcmp(name, entries[j].name))
            return entries[j].function;
    return NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!device || !name) return NULL;
    if ((!strcmp(name, "vkResetQueryPool") ||
         !strcmp(name, "vkResetQueryPoolEXT")) &&
        !(device->enabled_features_t09 & PS5VK_T09_FEATURE_HOST_QUERY_RESET))
        return NULL;
    if (buffer_device_address_command(name) &&
        !(device->enabled_features & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS))
        return NULL;
    if (device_group_command(name) && !device->device_group_extension_enabled)
        return NULL;
    if (timeline_semaphore_command(name) && !device->timeline_extension_enabled)
        return NULL;
    if (descriptor_update_template_command(name) &&
        !device->descriptor_update_template_extension_enabled)
        return NULL;
    if (create_renderpass2_command(name) && !device->create_renderpass2_extension_enabled)
        return NULL;
    if (swapchain_command(name) && !device->swapchain_extension_enabled)
        return NULL;
    if (memory_requirements2_command(name) && !device->memory_requirements2_extension_enabled)
        return NULL;
    if (bind_memory2_command(name) && !device->bind_memory2_extension_enabled)
        return NULL;
    if (maintenance4_command(name) && !device->maintenance4_extension_enabled)
        return NULL;
    if (extended_dynamic_state_command(name) &&
        !device->extended_dynamic_state_extension_enabled)
        return NULL;
    if (!strcmp(name, "vkTrimCommandPoolKHR") && !device->maintenance1_extension_enabled)
        return NULL;
    if (copy_commands2_command(name) && !device->copy_commands2_extension_enabled)
        return NULL;
    if ((!strcmp(name, "vkCmdBeginRenderingKHR") || !strcmp(name, "vkCmdEndRenderingKHR")) &&
        !device->dynamic_rendering_extension_enabled)
        return NULL;
    for (size_t j = 0; j < sizeof(entries) / sizeof(entries[0]); ++j)
        if (entries[j].scope == DEVICE && !strcmp(name, entries[j].name)) return entries[j].function;
    return NULL;
}
