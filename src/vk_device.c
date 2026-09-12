#include "vk_internal.h"
#include <string.h>

#define INVALID VK_ERROR_UNKNOWN
static int power_two(VkDeviceSize n) { return n && !(n & (n - 1)); }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!info || info->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO) return INVALID;
    if (info->enabledLayerCount) return VK_ERROR_LAYER_NOT_PRESENT;
    if (info->enabledExtensionCount) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info->pNext || info->flags) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->pApplicationInfo) {
        const VkApplicationInfo *a = info->pApplicationInfo;
        if (a->sType != VK_STRUCTURE_TYPE_APPLICATION_INFO || a->pNext) return INVALID;
        uint32_t version = a->apiVersion;
        if (version && (VK_API_VERSION_VARIANT(version) || VK_API_VERSION_MAJOR(version) != 1 ||
                        VK_API_VERSION_MINOR(version) != 0)) return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkInstance i = ps5vk_object_alloc(NULL, allocator, sizeof(*i),
        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE, &saved, &custom);
    if (!i) return VK_ERROR_OUT_OF_HOST_MEMORY;
    i->allocator = saved; i->custom_allocator = custom;
    VkResult result = ps5vk_platform_query(&i->physical.platform);
    if (result == VK_SUCCESS) {
        struct ps5vk_platform *p = &i->physical.platform;
        const VkPhysicalDeviceLimits *l = &p->properties.limits;
        const VkPhysicalDeviceMemoryProperties *m = &p->memory_properties;
        /* This frontend is intentionally one-memory-type/one-queue. Never
         * manufacture heap or alignment capabilities when platform data is absent. */
        if (!p->open || !p->close || !p->max_allocation ||
            !power_two(l->nonCoherentAtomSize) || !power_two(l->minStorageBufferOffsetAlignment) ||
            m->memoryTypeCount != 1 || m->memoryHeapCount != 1 || m->memoryTypes[0].heapIndex ||
            !(m->memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
            (m->memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            m->memoryHeaps[0].size < p->max_allocation)
            result = VK_ERROR_INITIALIZATION_FAILED;
    }
    if (result != VK_SUCCESS) { ps5vk_object_free(i, &saved, custom); return result; }
    i->physical.instance = i; *out = i;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance i, const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!i) return;
    if (i->devices) { ++i->lifetime_errors; return; }
    VkAllocationCallbacks a = i->allocator; VkBool32 custom = i->custom_allocator;
    ps5vk_object_free(i, &a, custom);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance i, uint32_t *count,
                                                         VkPhysicalDevice *out)
{
    if (!i || !count) return INVALID;
    if (!out) { *count = 1; return VK_SUCCESS; }
    if (!*count) return VK_INCOMPLETE;
    out[0] = &i->physical; *count = 1;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice p,
                                                         VkPhysicalDeviceProperties *out)
{ if (p && out) *out = p->platform.properties; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p,
                                                              VkPhysicalDeviceMemoryProperties *out)
{ if (p && out) *out = p->platform.memory_properties; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice p, VkPhysicalDeviceFeatures *out)
{ if (p && out) memset(out, 0, sizeof(*out)); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p,
    VkFormat format, VkFormatProperties *out)
{
    if (!out) return;
    *out = (VkFormatProperties){0};
    if (p && p->platform.format_properties)
        p->platform.format_properties(format, out);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice p,
    uint32_t *count, VkQueueFamilyProperties *out)
{
    if (!p || !count) return;
    if (!out) { *count = 1; return; }
    if (!*count) return;
    *out = (VkQueueFamilyProperties){.queueFlags = p->platform.queue_flags, .queueCount = 1,
                                     .minImageTransferGranularity = {1, 1, 1}};
    *count = 1;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice p,
    VkFormat format,VkImageType type,VkImageTiling tiling,VkImageUsageFlags usage,
    VkImageCreateFlags flags,VkImageFormatProperties *out)
{
    if(!out)return VK_ERROR_UNKNOWN;
    *out=(VkImageFormatProperties){0};
    if(!p || !p->platform.image_properties)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return p->platform.image_properties(format,type,tiling,usage,flags,
        p->platform.max_allocation,out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer,
    uint32_t *count, VkExtensionProperties *out)
{
    (void)out;
    if (!count) return INVALID;
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    *count = 0; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count,
                                                                 VkLayerProperties *out)
{ (void)out; if (!count) return INVALID; *count = 0; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p,
    const char *layer, uint32_t *count, VkExtensionProperties *out)
{
    if (!p) return INVALID;
    return vkEnumerateInstanceExtensionProperties(layer, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDevice *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!p || !info || info->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO) return INVALID;
    if (info->enabledExtensionCount) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info->pNext || info->flags) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->pEnabledFeatures) {
        /* VkPhysicalDeviceFeatures consists exclusively of VkBool32 fields;
         * copying words avoids aliasing another struct as a uint32_t array. */
        const unsigned char *bytes = (const unsigned char *)info->pEnabledFeatures;
        for (size_t j = 0; j < sizeof(*info->pEnabledFeatures); j += sizeof(VkBool32)) {
            VkBool32 enabled; memcpy(&enabled, bytes + j, sizeof(enabled));
            if (enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (info->queueCreateInfoCount != 1 || !info->pQueueCreateInfos) return INVALID;
    const VkDeviceQueueCreateInfo *q = info->pQueueCreateInfos;
    if (q->sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || q->pNext || q->flags ||
        q->queueFamilyIndex || q->queueCount != 1 || !q->pQueuePriorities ||
        !(q->pQueuePriorities[0] >= 0.0f && q->pQueuePriorities[0] <= 1.0f)) return INVALID;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkInstance i = p->instance;
    VkDevice d = ps5vk_object_alloc(i->custom_allocator ? &i->allocator : NULL,
        allocator, sizeof(*d), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &saved, &custom);
    if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->allocator = saved; d->custom_allocator = custom;
    VkResult result = p->platform.open(p->platform.context, &d->memory);
    if (result == VK_SUCCESS && (!d->memory.allocate || !d->memory.release ||
                                !d->memory.flush || !d->memory.invalidate)) {
        p->platform.close(&d->memory); result = VK_ERROR_INITIALIZATION_FAILED;
    }
    if (result != VK_SUCCESS) { ps5vk_object_free(d, &saved, custom); return result; }
    d->physical = p; d->queue.device = d; d->queue.next_serial = 1;
    d->compiler = p->platform.compiler;
    d->buffer_alignment = p->platform.properties.limits.minStorageBufferOffsetAlignment;
    d->noncoherent_atom = p->platform.properties.limits.nonCoherentAtomSize;
    d->max_allocation = p->platform.max_allocation;
    d->submit_backend = p->platform.queue_backend;
    d->progress = p->platform.progress;
    if (p->platform.configure) p->platform.configure(d);
    ++i->devices; *out = d;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice d, uint32_t family, uint32_t index, VkQueue *out)
{
    if (out) *out = d && !family && !index ? &d->queue : VK_NULL_HANDLE;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice d, const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!d) return;
    /* Valid usage requires children destroyed and work completed first. Defend
     * against invalid destruction by retaining ownership, not implicit frees. */
    if (d->memories || d->buffers || d->descriptor_objects || d->pipeline_objects || d->graphics_objects || d->command_pools || d->fences || d->submission ||
        d->queue.next_serial != d->queue.completed_serial + 1) {
        ++d->lifetime_errors; return;
    }
    d->physical->platform.close(&d->memory);
    --d->physical->instance->devices;
    VkAllocationCallbacks a = d->allocator; VkBool32 custom = d->custom_allocator;
    ps5vk_object_free(d, &a, custom);
}
