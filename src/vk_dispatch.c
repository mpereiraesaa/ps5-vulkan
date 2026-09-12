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
    ENTRY(vkGetPhysicalDeviceFormatProperties, INSTANCE),
    ENTRY(vkGetPhysicalDeviceImageFormatProperties, INSTANCE),
    ENTRY(vkGetPhysicalDeviceQueueFamilyProperties, INSTANCE),
    ENTRY(vkEnumerateDeviceExtensionProperties, INSTANCE),
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
    ENTRY(vkCreateBuffer, DEVICE),
    ENTRY(vkDestroyBuffer, DEVICE),
    ENTRY(vkCreateBufferView, DEVICE),
    ENTRY(vkDestroyBufferView, DEVICE),
    ENTRY(vkGetBufferMemoryRequirements, DEVICE),
    ENTRY(vkBindBufferMemory, DEVICE),
    ENTRY(vkCreateImage, DEVICE),
    ENTRY(vkDestroyImage, DEVICE),
    ENTRY(vkGetImageMemoryRequirements, DEVICE),
    ENTRY(vkBindImageMemory, DEVICE),
    ENTRY(vkCreateImageView, DEVICE),
    ENTRY(vkDestroyImageView, DEVICE),
    ENTRY(vkCreateSampler, DEVICE),
    ENTRY(vkDestroySampler, DEVICE),
    ENTRY(vkCreateRenderPass, DEVICE),
    ENTRY(vkDestroyRenderPass, DEVICE),
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
    ENTRY(vkCmdBindDescriptorSets, DEVICE),
    ENTRY(vkCmdDispatch, DEVICE),
    ENTRY(vkCmdBeginRenderPass, DEVICE),
    ENTRY(vkCmdEndRenderPass, DEVICE),
    ENTRY(vkCmdBindVertexBuffers, DEVICE),
    ENTRY(vkCmdBindIndexBuffer, DEVICE),
    ENTRY(vkCmdDraw, DEVICE),
    ENTRY(vkCmdDrawIndexed, DEVICE),
    ENTRY(vkCmdCopyBufferToImage, DEVICE),
    ENTRY(vkCmdPipelineBarrier, DEVICE),
    ENTRY(vkCreateFence, DEVICE),
    ENTRY(vkDestroyFence, DEVICE),
    ENTRY(vkResetFences, DEVICE),
    ENTRY(vkGetFenceStatus, DEVICE),
    ENTRY(vkWaitForFences, DEVICE),
    ENTRY(vkQueueSubmit, DEVICE),
    ENTRY(vkQueueWaitIdle, DEVICE),
    ENTRY(vkDeviceWaitIdle, DEVICE),
};
#undef ENTRY

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                              const char *name)
{
    if (!name) return NULL;
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
