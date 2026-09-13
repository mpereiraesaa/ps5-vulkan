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
    ENTRY(vkDestroyInstance, INSTANCE),
    ENTRY(vkEnumeratePhysicalDevices, INSTANCE),
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
    ENTRY(vkCreateImage, DEVICE),
    ENTRY(vkDestroyImage, DEVICE),
    ENTRY(vkGetImageMemoryRequirements, DEVICE),
    ENTRY(vkGetImageSubresourceLayout, DEVICE),
    ENTRY(vkBindImageMemory, DEVICE),
    ENTRY(vkCreateImageView, DEVICE),
    ENTRY(vkDestroyImageView, DEVICE),
    ENTRY(vkCreateSampler, DEVICE),
    ENTRY(vkDestroySampler, DEVICE),
    ENTRY(vkCreateRenderPass, DEVICE),
    ENTRY(vkDestroyRenderPass, DEVICE),
    ENTRY(vkGetRenderAreaGranularity, DEVICE),
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
    ENTRY(vkCmdDispatchIndirect, DEVICE),
    ENTRY(vkCmdBeginRenderPass, DEVICE),
    ENTRY(vkCmdNextSubpass, DEVICE),
    ENTRY(vkCmdEndRenderPass, DEVICE),
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
    ENTRY(vkCmdResetQueryPool, DEVICE),
    ENTRY(vkCmdBeginQuery, DEVICE),
    ENTRY(vkCmdEndQuery, DEVICE),
    ENTRY(vkCmdWriteTimestamp, DEVICE),
    ENTRY(vkCmdCopyQueryPoolResults, DEVICE),
    ENTRY(vkGetImageSparseMemoryRequirements, DEVICE),
    ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties, INSTANCE),
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

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                              const char *name)
{
    if (!name) return NULL;
    if (gpdp2_command(name) &&
        (!instance || !instance->features2_extension_enabled))
        return NULL;
    for (size_t j = 0; j < sizeof(entries) / sizeof(entries[0]); ++j)
        if ((instance || entries[j].scope == GLOBAL) && !strcmp(name, entries[j].name))
            return entries[j].function;
    return NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!device || !name) return NULL;
    for (size_t j = 0; j < sizeof(entries) / sizeof(entries[0]); ++j)
        if (entries[j].scope == DEVICE && !strcmp(name, entries[j].name)) return entries[j].function;
    return NULL;
}
