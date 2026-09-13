#include "vk_internal.h"
#include "compilation_cache.h"
#include "physical_device_profile.h"
#include <string.h>

#define INVALID VK_ERROR_UNKNOWN

static const VkExtensionProperties instance_extensions[] = {
    {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
     VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION},
};

static VkResult enumerate_extensions(const VkExtensionProperties *properties,
    uint32_t total, uint32_t *count, VkExtensionProperties *out)
{
    if (!count) return INVALID;
    if (!out) { *count = total; return VK_SUCCESS; }
    const uint32_t requested = *count;
    const uint32_t written = requested < total ? requested : total;
    if (written) memcpy(out, properties, written * sizeof(*out));
    *count = written;
    return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static int all_core_features_disabled(const VkPhysicalDeviceFeatures *features)
{
    if (!features) return 1;
    const unsigned char *bytes = (const unsigned char *)features;
    for (size_t offset = 0; offset < sizeof(*features); offset += sizeof(VkBool32)) {
        VkBool32 enabled;
        memcpy(&enabled, bytes + offset, sizeof(enabled));
        if (enabled) return 0;
    }
    return 1;
}

static int valid_bool(VkBool32 value)
{ return value == VK_FALSE || value == VK_TRUE; }

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!info || info->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO) return INVALID;
    if (info->enabledLayerCount) return VK_ERROR_LAYER_NOT_PRESENT;
    VkBool32 features2_enabled = VK_FALSE;
    if (info->enabledExtensionCount && !info->ppEnabledExtensionNames) return INVALID;
    for (uint32_t n = 0; n < info->enabledExtensionCount; ++n) {
        const char *name = info->ppEnabledExtensionNames[n];
        if (!name) return INVALID;
        if (strcmp(name, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        if (features2_enabled) return INVALID;
        features2_enabled = VK_TRUE;
    }
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
    i->features2_extension_enabled = features2_enabled;
    VkResult result = ps5vk_platform_query(&i->physical.platform);
    if (result == VK_SUCCESS) {
        struct ps5vk_platform *p = &i->physical.platform;
        if (!p->open || !p->close || !p->max_allocation ||
            !ps5vk_physical_profile_valid(&p->properties, &p->memory_properties,
                p->max_allocation, p->queue_flags, !!p->format_properties,
                !!p->image_properties))
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
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice p,
                                                           VkPhysicalDeviceFeatures2 *out)
{
    if (!p || !out || out->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
        return;
    memset(&out->features, 0, sizeof(out->features));
    for (VkBaseOutStructure *next = (VkBaseOutStructure *)out->pNext; next;
         next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES) {
            VkPhysicalDevice8BitStorageFeatures *features =
                (VkPhysicalDevice8BitStorageFeatures *)next;
            features->storageBuffer8BitAccess =
                !!(p->platform.supported_features & PS5VK_FEATURE_STORAGE_BUFFER_8BIT);
            features->uniformAndStorageBuffer8BitAccess = VK_FALSE;
            features->storagePushConstant8 = VK_FALSE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES) {
            VkPhysicalDevice16BitStorageFeatures *features =
                (VkPhysicalDevice16BitStorageFeatures *)next;
            features->storageBuffer16BitAccess =
                !!(p->platform.supported_features & PS5VK_FEATURE_STORAGE_BUFFER_16BIT);
            features->uniformAndStorageBuffer16BitAccess = VK_FALSE;
            features->storagePushConstant16 = VK_FALSE;
            features->storageInputOutput16 = VK_FALSE;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES) {
            ((VkPhysicalDeviceProtectedMemoryFeatures *)next)->protectedMemory = VK_FALSE;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES) {
            ((VkPhysicalDeviceShaderDrawParametersFeatures *)next)->shaderDrawParameters = VK_FALSE;
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice p,
                                                              VkPhysicalDeviceProperties2 *out)
{
    if (!p || !out || out->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2)
        return;
    vkGetPhysicalDeviceProperties(p, &out->properties);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice p,
    VkPhysicalDeviceMemoryProperties2 *out)
{
    if (!p || !out || out->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2)
        return;
    vkGetPhysicalDeviceMemoryProperties(p, &out->memoryProperties);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p,
    VkFormat format, VkFormatProperties *out)
{
    if (!out) return;
    *out = (VkFormatProperties){0};
    if (p && p->platform.format_properties)
        p->platform.format_properties(format, out);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice p,
    VkFormat format, VkFormatProperties2 *out)
{
    if (!p || !out || out->sType != VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2)
        return;
    vkGetPhysicalDeviceFormatProperties(p, format, &out->formatProperties);
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
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice p,
    uint32_t *count, VkQueueFamilyProperties2 *out)
{
    if (!p || !count) return;
    if (!out) { *count = 1; return; }
    if (!*count) return;
    if (out[0].sType != VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2) {
        *count = 0;
        return;
    }
    uint32_t one = 1;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &one, &out[0].queueFamilyProperties);
    *count = one;
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
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice p,
    const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out)
{
    if (!p || !info || !out ||
        info->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 ||
        out->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2)
        return INVALID;
    VkResult result = vkGetPhysicalDeviceImageFormatProperties(p, info->format, info->type,
        info->tiling, info->usage, info->flags, &out->imageFormatProperties);
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2KHR(
    VkPhysicalDevice p, const VkPhysicalDeviceSparseImageFormatInfo2 *info,
    uint32_t *count, VkSparseImageFormatProperties2 *out)
{
    (void)out;
    if (!p || !info || !count ||
        info->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2)
        return;
    /* sparseBinding is not advertised, so the valid report is an empty list. */
    *count = 0;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer,
    uint32_t *count, VkExtensionProperties *out)
{
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    return enumerate_extensions(instance_extensions,
        (uint32_t)(sizeof(instance_extensions) / sizeof(instance_extensions[0])),
        count, out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count,
                                                                 VkLayerProperties *out)
{ (void)out; if (!count) return INVALID; *count = 0; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p,
    const char *layer, uint32_t *count, VkExtensionProperties *out)
{
    if (!p || !count) return INVALID;
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    VkExtensionProperties properties[3];
    uint32_t total = 0;
    if (p->platform.supported_features & (PS5VK_FEATURE_STORAGE_BUFFER_8BIT |
                                          PS5VK_FEATURE_STORAGE_BUFFER_16BIT)) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
            VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_SPEC_VERSION};
    }
    if (p->platform.supported_features & PS5VK_FEATURE_STORAGE_BUFFER_8BIT) {
        properties[total++] = (VkExtensionProperties){VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
                                                       VK_KHR_8BIT_STORAGE_SPEC_VERSION};
    }
    if (p->platform.supported_features & PS5VK_FEATURE_STORAGE_BUFFER_16BIT) {
        properties[total++] = (VkExtensionProperties){VK_KHR_16BIT_STORAGE_EXTENSION_NAME,
                                                       VK_KHR_16BIT_STORAGE_SPEC_VERSION};
    }
    return enumerate_extensions(properties, total, count, out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
    uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
    (void)pProperties;
    if (!physicalDevice || !pPropertyCount) return INVALID;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDevice *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!p || !info || info->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO) {
        return INVALID;
    }
    if (info->flags) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->enabledExtensionCount && !info->ppEnabledExtensionNames) {
        return INVALID;
    }
    VkBool32 storage_class = VK_FALSE, extension8 = VK_FALSE, extension16 = VK_FALSE;
    for (uint32_t n = 0; n < info->enabledExtensionCount; ++n) {
        const char *name = info->ppEnabledExtensionNames[n];
        VkBool32 *seen = NULL;
        if (!name) return INVALID;
        if (!strcmp(name, VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME))
            seen = &storage_class;
        else if (!strcmp(name, VK_KHR_8BIT_STORAGE_EXTENSION_NAME))
            seen = &extension8;
        else if (!strcmp(name, VK_KHR_16BIT_STORAGE_EXTENSION_NAME))
            seen = &extension16;
        else {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        if (*seen) return INVALID;
        *seen = VK_TRUE;
    }
    if ((extension8 && !(p->platform.supported_features & PS5VK_FEATURE_STORAGE_BUFFER_8BIT)) ||
        (extension16 && !(p->platform.supported_features & PS5VK_FEATURE_STORAGE_BUFFER_16BIT)) ||
        ((extension8 || extension16) &&
         (!storage_class || !p->instance->features2_extension_enabled)))
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    uint32_t enabled_features = 0;
    VkBool32 saw_features2 = VK_FALSE, saw8 = VK_FALSE, saw16 = VK_FALSE;
    for (const VkBaseInStructure *next = (const VkBaseInStructure *)info->pNext;
         next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            if (saw_features2 || info->pEnabledFeatures) return INVALID;
            saw_features2 = VK_TRUE;
            const VkPhysicalDeviceFeatures2 *features =
                (const VkPhysicalDeviceFeatures2 *)next;
            if (!all_core_features_disabled(&features->features))
                return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES) {
            if (saw8 || !extension8) return VK_ERROR_FEATURE_NOT_PRESENT;
            saw8 = VK_TRUE;
            const VkPhysicalDevice8BitStorageFeatures *features =
                (const VkPhysicalDevice8BitStorageFeatures *)next;
            if (!valid_bool(features->storageBuffer8BitAccess) ||
                !valid_bool(features->uniformAndStorageBuffer8BitAccess) ||
                !valid_bool(features->storagePushConstant8)) return INVALID;
            if (features->uniformAndStorageBuffer8BitAccess || features->storagePushConstant8)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if (features->storageBuffer8BitAccess)
                enabled_features |= PS5VK_FEATURE_STORAGE_BUFFER_8BIT;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES) {
            if (saw16 || !extension16) return VK_ERROR_FEATURE_NOT_PRESENT;
            saw16 = VK_TRUE;
            const VkPhysicalDevice16BitStorageFeatures *features =
                (const VkPhysicalDevice16BitStorageFeatures *)next;
            if (!valid_bool(features->storageBuffer16BitAccess) ||
                !valid_bool(features->uniformAndStorageBuffer16BitAccess) ||
                !valid_bool(features->storagePushConstant16) ||
                !valid_bool(features->storageInputOutput16)) return INVALID;
            if (features->uniformAndStorageBuffer16BitAccess ||
                features->storagePushConstant16 || features->storageInputOutput16)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if (features->storageBuffer16BitAccess)
                enabled_features |= PS5VK_FEATURE_STORAGE_BUFFER_16BIT;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES) {
            const VkPhysicalDeviceProtectedMemoryFeatures *features =
                (const VkPhysicalDeviceProtectedMemoryFeatures *)next;
            /* The pinned CTS includes this core-1.1 feature structure, in its
             * default device chain even for a Vulkan-1.0 implementation once
             * Features2 is available. Accept the neutral value only: protected
             * memory remains unadvertised and requesting it fails closed. */
            if (!valid_bool(features->protectedMemory)) return INVALID;
            if (features->protectedMemory) return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES) {
            const VkPhysicalDeviceShaderDrawParametersFeatures *features =
                (const VkPhysicalDeviceShaderDrawParametersFeatures *)next;
            /* Same neutral-chain rule as protected memory. This 1.1 feature is
             * not advertised by the Vulkan-1.0 profile. */
            if (!valid_bool(features->shaderDrawParameters)) return INVALID;
            if (features->shaderDrawParameters) return VK_ERROR_FEATURE_NOT_PRESENT;
        } else {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (!all_core_features_disabled(info->pEnabledFeatures)) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (info->queueCreateInfoCount != 1 || !info->pQueueCreateInfos) {
        return INVALID;
    }
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
    d->enabled_features = enabled_features;
    d->compiler = p->platform.compiler;
    d->buffer_alignment = p->platform.properties.limits.minStorageBufferOffsetAlignment;
    d->uniform_buffer_alignment = p->platform.properties.limits.minUniformBufferOffsetAlignment;
    d->noncoherent_atom = p->platform.properties.limits.nonCoherentAtomSize;
    d->max_allocation = p->platform.max_allocation;
    d->submit_backend = p->platform.queue_backend;
    d->progress = p->platform.progress;
    d->pipeline_cache = ps5vk_compilation_cache_create(64, 4 * 1024 * 1024);
    if (p->platform.configure) {
        p->platform.configure(d);
    }
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
    if (d->memories || d->buffers || d->buffer_views || d->descriptor_objects || d->pipeline_objects || d->graphics_objects || d->command_pools || d->fences || d->pipeline_caches || d->submission ||
        d->queue.next_serial != d->queue.completed_serial + 1) {
        ++d->lifetime_errors; return;
    }
    if (d->pipeline_cache) {
        ps5vk_compilation_cache_destroy(d->pipeline_cache);
        d->pipeline_cache = NULL;
    }
    d->physical->platform.close(&d->memory);
    --d->physical->instance->devices;
    VkAllocationCallbacks a = d->allocator; VkBool32 custom = d->custom_allocator;
    ps5vk_object_free(d, &a, custom);
}

void ps5vk_device_enable_runtime_compiler(VkDevice device)
{
    if (device) device->runtime_compiler_enabled = VK_TRUE;
}
