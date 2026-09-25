/* Vulkan binding and call trace for the DXVK native payload.
 *
 * DXVK's native loader opens "libvulkan.so" with dlopen and looks up
 * vkGetInstanceProcAddr with dlsym. The payload links ps5vk statically, so the
 * payload provides dlopen/dlsym/dlclose: the Vulkan library names resolve to a
 * sentinel handle whose vkGetInstanceProcAddr is ps5vk's own entry point,
 * reached through a tracing layer. Every other library name is refused (the
 * payload contains no other loadable module) and logged. The payload defines
 * the dl* entry points itself; no system loader or libc dlfcn is linked.
 *
 * The tracing layer forwards every command unchanged. It logs entry points
 * that ps5vk does not provide, the parameters of instance/device/resource
 * creation, and every negative VkResult with its call and parameters. */
#include "telemetry.h"

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
void *dlopen(const char *name, int flags);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);
int dladdr(const void *address, void *info);
}

namespace {

std::atomic<unsigned> g_calls{0}, g_refusals{0}, g_missing{0};
char g_vulkan_handle_storage;
void *const g_vulkan_handle = &g_vulkan_handle_storage;

const char *result_name(VkResult r)
{
    switch (r) {
#define R(x) case x: return #x;
    R(VK_SUCCESS) R(VK_NOT_READY) R(VK_TIMEOUT) R(VK_EVENT_SET) R(VK_EVENT_RESET)
    R(VK_INCOMPLETE) R(VK_ERROR_OUT_OF_HOST_MEMORY) R(VK_ERROR_OUT_OF_DEVICE_MEMORY)
    R(VK_ERROR_INITIALIZATION_FAILED) R(VK_ERROR_DEVICE_LOST) R(VK_ERROR_MEMORY_MAP_FAILED)
    R(VK_ERROR_LAYER_NOT_PRESENT) R(VK_ERROR_EXTENSION_NOT_PRESENT)
    R(VK_ERROR_FEATURE_NOT_PRESENT) R(VK_ERROR_INCOMPATIBLE_DRIVER)
    R(VK_ERROR_TOO_MANY_OBJECTS) R(VK_ERROR_FORMAT_NOT_SUPPORTED)
    R(VK_ERROR_FRAGMENTED_POOL) R(VK_ERROR_UNKNOWN) R(VK_ERROR_OUT_OF_POOL_MEMORY)
    R(VK_ERROR_INVALID_EXTERNAL_HANDLE) R(VK_ERROR_FRAGMENTATION)
    R(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS) R(VK_ERROR_SURFACE_LOST_KHR)
    R(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) R(VK_SUBOPTIMAL_KHR) R(VK_ERROR_OUT_OF_DATE_KHR)
#undef R
    default: return "VK_RESULT_OTHER";
    }
}

/* Bounded string builder; silently truncates. */
struct Text {
    char data[900];
    size_t used = 0;
    Text() { data[0] = 0; }
    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (used + 1 >= sizeof(data)) return;
        va_list args;
        va_start(args, fmt);
        int n = std::vsnprintf(data + used, sizeof(data) - used, fmt, args);
        va_end(args);
        if (n > 0) used = used + size_t(n) < sizeof(data) ? used + size_t(n) : sizeof(data) - 1;
    }
    void chain(const void *next) {
        add(" chain=");
        int count = 0;
        for (auto *s = static_cast<const VkBaseInStructure *>(next); s && count < 24;
             s = s->pNext, ++count)
            add("%s%u", count ? "," : "", unsigned(s->sType));
        if (!count) add("none");
    }
    void names(const char *label, uint32_t count, const char *const *names) {
        add(" %s=", label);
        for (uint32_t i = 0; i < count; ++i)
            add("%s%s", i ? "," : "", names && names[i] ? names[i] : "(null)");
        if (!count) add("none");
    }
};

/* Log one traced call. Successful calls are logged when `verbose`; every
 * negative result is a refusal candidate. */
VkResult record(const char *call, VkResult result, const Text &params, bool verbose)
{
    ++g_calls;
    dxvk_telemetry_last_call(call, params.data);
    if (result < 0) {
        ++g_refusals;
        dxvk_telemetry_emit("ERR", "DXVK_VK_REFUSAL call=%s result=%d(%s) stage=%s%s", call,
                            int(result), result_name(result), dxvk_telemetry_open_stage(),
                            params.data);
        dxvk_telemetry_refusal("vulkan", call, int(result), params.data);
    } else if (verbose) {
        dxvk_telemetry_emit("INFO", "DXVK_VK_CALL call=%s result=%d(%s)%s", call, int(result),
                            result_name(result), params.data);
    }
    return result;
}

void emit_extensions(const char *call, uint32_t count, const VkExtensionProperties *props)
{
    /* Split long lists so each record stays below the ps5log line limit. */
    Text line;
    uint32_t part = 0;
    for (uint32_t i = 0; i < count; ++i) {
        line.add("%s%s:%u", line.used ? "," : "", props[i].extensionName, props[i].specVersion);
        if (line.used > 600 || i + 1 == count) {
            dxvk_telemetry_emit("INFO", "DXVK_VK_EXTENSIONS call=%s part=%u count=%u list=%s",
                                call, part++, count, line.data);
            line = Text();
        }
    }
    if (!count)
        dxvk_telemetry_emit("INFO", "DXVK_VK_EXTENSIONS call=%s part=0 count=0 list=none", call);
}

#define REAL(name) PFN_##name real_##name = nullptr

REAL(vkEnumerateInstanceExtensionProperties);
REAL(vkEnumerateInstanceVersion);
REAL(vkCreateInstance);
REAL(vkEnumeratePhysicalDevices);
REAL(vkGetPhysicalDeviceProperties);
REAL(vkGetPhysicalDeviceProperties2);
REAL(vkGetPhysicalDeviceFeatures2);
REAL(vkEnumerateDeviceExtensionProperties);
REAL(vkCreateDevice);
REAL(vkGetDeviceProcAddr);
REAL(vkCreateImage);
REAL(vkCreateImageView);
REAL(vkCreateBuffer);
REAL(vkCreateBufferView);
REAL(vkAllocateMemory);
REAL(vkBindImageMemory);
REAL(vkBindBufferMemory);
REAL(vkBindImageMemory2);
REAL(vkBindBufferMemory2);
REAL(vkMapMemory);
REAL(vkCreateShaderModule);
REAL(vkCreateGraphicsPipelines);
REAL(vkCreateComputePipelines);
REAL(vkCreatePipelineLayout);
REAL(vkCreateDescriptorSetLayout);
REAL(vkCreateDescriptorPool);
REAL(vkAllocateDescriptorSets);
REAL(vkCreateDescriptorUpdateTemplate);
REAL(vkCreateSampler);
REAL(vkCreateSemaphore);
REAL(vkCreateFence);
REAL(vkCreateEvent);
REAL(vkCreateQueryPool);
REAL(vkCreatePipelineCache);
REAL(vkCreateCommandPool);
REAL(vkAllocateCommandBuffers);
REAL(vkBeginCommandBuffer);
REAL(vkEndCommandBuffer);
REAL(vkQueueSubmit);
REAL(vkQueueSubmit2);
REAL(vkQueueWaitIdle);
REAL(vkDeviceWaitIdle);
REAL(vkWaitForFences);
REAL(vkWaitSemaphores);
REAL(vkCreateRenderPass);
REAL(vkCreateRenderPass2);
REAL(vkCreateFramebuffer);

/* ---- global and instance commands ---- */

VKAPI_ATTR VkResult VKAPI_CALL t_vkEnumerateInstanceVersion(uint32_t *version)
{
    VkResult r = real_vkEnumerateInstanceVersion(version);
    Text t;
    if (r == VK_SUCCESS && version)
        t.add(" version=%u.%u.%u", VK_API_VERSION_MAJOR(*version),
              VK_API_VERSION_MINOR(*version), VK_API_VERSION_PATCH(*version));
    return record("vkEnumerateInstanceVersion", r, t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkEnumerateInstanceExtensionProperties(
    const char *layer, uint32_t *count, VkExtensionProperties *props)
{
    static std::atomic<bool> begun{false};
    if (!begun.exchange(true))
        dxvk_telemetry_stage("dxvk.instance", "begin", "first_call=vkEnumerateInstanceExtensionProperties");
    VkResult r = real_vkEnumerateInstanceExtensionProperties(layer, count, props);
    Text t;
    t.add(" layer=%s count=%u query=%d", layer ? layer : "none", count ? *count : 0u, props == nullptr);
    if (r >= 0 && props && count) emit_extensions("vkEnumerateInstanceExtensionProperties", *count, props);
    return record("vkEnumerateInstanceExtensionProperties", r, t, props == nullptr);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateInstance(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    Text t;
    if (info) {
        const VkApplicationInfo *app = info->pApplicationInfo;
        if (app)
            t.add(" apiVersion=%u.%u.%u app=%s engine=%s engineVersion=0x%x",
                  VK_API_VERSION_MAJOR(app->apiVersion), VK_API_VERSION_MINOR(app->apiVersion),
                  VK_API_VERSION_PATCH(app->apiVersion),
                  app->pApplicationName ? app->pApplicationName : "(null)",
                  app->pEngineName ? app->pEngineName : "(null)", app->engineVersion);
        t.add(" flags=0x%x layers=%u", info->flags, info->enabledLayerCount);
        t.names("extensions", info->enabledExtensionCount, info->ppEnabledExtensionNames);
        t.chain(info->pNext);
    }
    VkResult r = real_vkCreateInstance(info, allocator, instance);
    record("vkCreateInstance", r, t, true);
    dxvk_telemetry_stage("dxvk.instance", r == VK_SUCCESS ? "ok" : "fail", t.data);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkEnumeratePhysicalDevices(VkInstance instance,
    uint32_t *count, VkPhysicalDevice *devices)
{
    static std::atomic<bool> begun{false};
    if (!begun.exchange(true))
        dxvk_telemetry_stage("dxvk.adapter_enum", "begin", "");
    VkResult r = real_vkEnumeratePhysicalDevices(instance, count, devices);
    Text t;
    t.add(" count=%u query=%d", count ? *count : 0u, devices == nullptr);
    return record("vkEnumeratePhysicalDevices", r, t, true);
}

VKAPI_ATTR void VKAPI_CALL t_vkGetPhysicalDeviceProperties(VkPhysicalDevice device,
    VkPhysicalDeviceProperties *props)
{
    static std::atomic<unsigned> logged{0};
    real_vkGetPhysicalDeviceProperties(device, props);
    ++g_calls;
    if (props && logged++ < 2)
        dxvk_telemetry_emit("INFO",
            "DXVK_VK_PROPERTIES apiVersion=%u.%u.%u driverVersion=0x%x vendorID=0x%x deviceID=0x%x deviceType=%u deviceName=%s",
            VK_API_VERSION_MAJOR(props->apiVersion), VK_API_VERSION_MINOR(props->apiVersion),
            VK_API_VERSION_PATCH(props->apiVersion), props->driverVersion, props->vendorID,
            props->deviceID, unsigned(props->deviceType), props->deviceName);
}

VKAPI_ATTR void VKAPI_CALL t_vkGetPhysicalDeviceProperties2(VkPhysicalDevice device,
    VkPhysicalDeviceProperties2 *props)
{
    static std::atomic<unsigned> logged{0};
    Text t;
    if (props) t.chain(props->pNext);
    real_vkGetPhysicalDeviceProperties2(device, props);
    ++g_calls;
    dxvk_telemetry_last_call("vkGetPhysicalDeviceProperties2", t.data);
    if (logged++ < 4)
        dxvk_telemetry_emit("INFO", "DXVK_VK_CALL call=vkGetPhysicalDeviceProperties2 result=void%s", t.data);
}

VKAPI_ATTR void VKAPI_CALL t_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice device,
    VkPhysicalDeviceFeatures2 *features)
{
    static std::atomic<unsigned> logged{0};
    Text t;
    if (features) t.chain(features->pNext);
    real_vkGetPhysicalDeviceFeatures2(device, features);
    ++g_calls;
    dxvk_telemetry_last_call("vkGetPhysicalDeviceFeatures2", t.data);
    if (logged++ < 4)
        dxvk_telemetry_emit("INFO", "DXVK_VK_CALL call=vkGetPhysicalDeviceFeatures2 result=void%s", t.data);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice device,
    const char *layer, uint32_t *count, VkExtensionProperties *props)
{
    static std::atomic<unsigned> listed{0};
    VkResult r = real_vkEnumerateDeviceExtensionProperties(device, layer, count, props);
    Text t;
    t.add(" layer=%s count=%u query=%d", layer ? layer : "none", count ? *count : 0u, props == nullptr);
    if (r >= 0 && props && count && listed++ < 1)
        emit_extensions("vkEnumerateDeviceExtensionProperties", *count, props);
    return record("vkEnumerateDeviceExtensionProperties", r, t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateDevice(VkPhysicalDevice physical,
    const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *device)
{
    if (!std::strcmp(dxvk_telemetry_open_stage(), "dxvk.adapter_enum"))
        dxvk_telemetry_stage("dxvk.adapter_enum", "ok", "adapter=selected");
    dxvk_telemetry_stage("dxvk.device_create", "begin", "");
    Text t;
    if (info) {
        t.add(" flags=0x%x queues=", info->flags);
        for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
            t.add("%s%u:%u", i ? "," : "", info->pQueueCreateInfos[i].queueFamilyIndex,
                  info->pQueueCreateInfos[i].queueCount);
        t.names("extensions", info->enabledExtensionCount, info->ppEnabledExtensionNames);
        t.add(" pEnabledFeatures=%d", info->pEnabledFeatures != nullptr);
        t.chain(info->pNext);
        /* Core feature bits requested through VkPhysicalDeviceFeatures2. */
        for (auto *s = static_cast<const VkBaseInStructure *>(info->pNext); s; s = s->pNext) {
            if (s->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) continue;
            const auto *f = &reinterpret_cast<const VkPhysicalDeviceFeatures2 *>(s)->features;
            const VkBool32 *bits = reinterpret_cast<const VkBool32 *>(f);
            uint64_t mask = 0;
            for (size_t i = 0; i < sizeof(*f) / sizeof(VkBool32); ++i)
                if (bits[i]) mask |= uint64_t(1) << i;
            t.add(" core_features=0x%llx", static_cast<unsigned long long>(mask));
        }
    }
    VkResult r = real_vkCreateDevice(physical, info, allocator, device);
    record("vkCreateDevice", r, t, true);
    dxvk_telemetry_stage("dxvk.device_create", r == VK_SUCCESS ? "ok" : "fail", "");
    return r;
}

/* ---- device commands ---- */

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateImage(VkDevice d, const VkImageCreateInfo *i,
    const VkAllocationCallbacks *a, VkImage *o)
{
    Text t;
    if (i) {
        t.add(" type=%u format=%u extent=%ux%ux%u mips=%u layers=%u samples=%u tiling=%u usage=0x%x flags=0x%x sharing=%u layout=%u",
              unsigned(i->imageType), unsigned(i->format), i->extent.width, i->extent.height,
              i->extent.depth, i->mipLevels, i->arrayLayers, unsigned(i->samples),
              unsigned(i->tiling), i->usage, i->flags, unsigned(i->sharingMode),
              unsigned(i->initialLayout));
        t.chain(i->pNext);
    }
    return record("vkCreateImage", real_vkCreateImage(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *i,
    const VkAllocationCallbacks *a, VkImageView *o)
{
    Text t;
    if (i) {
        t.add(" viewType=%u format=%u aspect=0x%x mips=%u+%u layers=%u+%u flags=0x%x",
              unsigned(i->viewType), unsigned(i->format), i->subresourceRange.aspectMask,
              i->subresourceRange.baseMipLevel, i->subresourceRange.levelCount,
              i->subresourceRange.baseArrayLayer, i->subresourceRange.layerCount, i->flags);
        t.chain(i->pNext);
    }
    return record("vkCreateImageView", real_vkCreateImageView(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *i,
    const VkAllocationCallbacks *a, VkBuffer *o)
{
    Text t;
    if (i) {
        t.add(" size=%llu usage=0x%x flags=0x%x sharing=%u",
              static_cast<unsigned long long>(i->size), i->usage, i->flags, unsigned(i->sharingMode));
        t.chain(i->pNext);
    }
    return record("vkCreateBuffer", real_vkCreateBuffer(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateBufferView(VkDevice d, const VkBufferViewCreateInfo *i,
    const VkAllocationCallbacks *a, VkBufferView *o)
{
    Text t;
    if (i) {
        t.add(" format=%u offset=%llu range=%llu", unsigned(i->format),
              static_cast<unsigned long long>(i->offset), static_cast<unsigned long long>(i->range));
        t.chain(i->pNext);
    }
    return record("vkCreateBufferView", real_vkCreateBufferView(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *i,
    const VkAllocationCallbacks *a, VkDeviceMemory *o)
{
    Text t;
    if (i) {
        t.add(" size=%llu type=%u", static_cast<unsigned long long>(i->allocationSize),
              i->memoryTypeIndex);
        t.chain(i->pNext);
    }
    return record("vkAllocateMemory", real_vkAllocateMemory(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkBindImageMemory(VkDevice d, VkImage i, VkDeviceMemory m,
    VkDeviceSize offset)
{
    Text t;
    t.add(" offset=%llu", static_cast<unsigned long long>(offset));
    return record("vkBindImageMemory", real_vkBindImageMemory(d, i, m, offset), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m,
    VkDeviceSize offset)
{
    Text t;
    t.add(" offset=%llu", static_cast<unsigned long long>(offset));
    return record("vkBindBufferMemory", real_vkBindBufferMemory(d, b, m, offset), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkBindImageMemory2(VkDevice d, uint32_t n,
    const VkBindImageMemoryInfo *i)
{
    Text t;
    t.add(" count=%u", n);
    if (n && i) t.chain(i[0].pNext);
    return record("vkBindImageMemory2", real_vkBindImageMemory2(d, n, i), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkBindBufferMemory2(VkDevice d, uint32_t n,
    const VkBindBufferMemoryInfo *i)
{
    Text t;
    t.add(" count=%u", n);
    if (n && i) t.chain(i[0].pNext);
    return record("vkBindBufferMemory2", real_vkBindBufferMemory2(d, n, i), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **data)
{
    Text t;
    t.add(" offset=%llu size=%llu flags=0x%x", static_cast<unsigned long long>(offset),
          static_cast<unsigned long long>(size), flags);
    return record("vkMapMemory", real_vkMapMemory(d, m, offset, size, flags, data), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateShaderModule(VkDevice d,
    const VkShaderModuleCreateInfo *i, const VkAllocationCallbacks *a, VkShaderModule *o)
{
    Text t;
    if (i) {
        uint32_t hash = 2166136261u;
        const uint8_t *code = reinterpret_cast<const uint8_t *>(i->pCode);
        for (size_t n = 0; code && n < i->codeSize; ++n) hash = (hash ^ code[n]) * 16777619u;
        t.add(" codeSize=%zu fnv1a=%08x", i->codeSize, hash);
        t.chain(i->pNext);
    }
    return record("vkCreateShaderModule", real_vkCreateShaderModule(d, i, a, o), t, true);
}

void describe_stages(Text &t, uint32_t count, const VkPipelineShaderStageCreateInfo *stages)
{
    t.add(" stages=");
    for (uint32_t s = 0; s < count && stages; ++s)
        t.add("%s0x%x", s ? "," : "", unsigned(stages[s].stage));
    if (!count) t.add("none");
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache c,
    uint32_t n, const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a,
    VkPipeline *o)
{
    Text t;
    t.add(" count=%u", n);
    if (n && i) {
        t.add(" flags=0x%x renderPass=%d subpass=%u dynamicStates=%u", i[0].flags,
              i[0].renderPass != VK_NULL_HANDLE, i[0].subpass,
              i[0].pDynamicState ? i[0].pDynamicState->dynamicStateCount : 0u);
        describe_stages(t, i[0].stageCount, i[0].pStages);
        t.chain(i[0].pNext);
    }
    return record("vkCreateGraphicsPipelines", real_vkCreateGraphicsPipelines(d, c, n, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateComputePipelines(VkDevice d, VkPipelineCache c,
    uint32_t n, const VkComputePipelineCreateInfo *i, const VkAllocationCallbacks *a,
    VkPipeline *o)
{
    Text t;
    t.add(" count=%u", n);
    if (n && i) {
        t.add(" flags=0x%x", i[0].flags);
        t.chain(i[0].pNext);
    }
    return record("vkCreateComputePipelines", real_vkCreateComputePipelines(d, c, n, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreatePipelineLayout(VkDevice d,
    const VkPipelineLayoutCreateInfo *i, const VkAllocationCallbacks *a, VkPipelineLayout *o)
{
    Text t;
    if (i) {
        t.add(" flags=0x%x sets=%u pushRanges=%u", i->flags, i->setLayoutCount,
              i->pushConstantRangeCount);
        for (uint32_t r = 0; r < i->pushConstantRangeCount; ++r)
            t.add(" push%u=0x%x:%u+%u", r, i->pPushConstantRanges[r].stageFlags,
                  i->pPushConstantRanges[r].offset, i->pPushConstantRanges[r].size);
    }
    return record("vkCreatePipelineLayout", real_vkCreatePipelineLayout(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateDescriptorSetLayout(VkDevice d,
    const VkDescriptorSetLayoutCreateInfo *i, const VkAllocationCallbacks *a,
    VkDescriptorSetLayout *o)
{
    Text t;
    if (i) {
        t.add(" flags=0x%x bindings=", i->flags);
        for (uint32_t b = 0; b < i->bindingCount && i->pBindings; ++b)
            t.add("%s%u:%u:%u:0x%x", b ? "," : "", i->pBindings[b].binding,
                  unsigned(i->pBindings[b].descriptorType), i->pBindings[b].descriptorCount,
                  i->pBindings[b].stageFlags);
        if (!i->bindingCount) t.add("none");
        t.chain(i->pNext);
    }
    return record("vkCreateDescriptorSetLayout", real_vkCreateDescriptorSetLayout(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateDescriptorPool(VkDevice d,
    const VkDescriptorPoolCreateInfo *i, const VkAllocationCallbacks *a, VkDescriptorPool *o)
{
    Text t;
    if (i) {
        t.add(" flags=0x%x maxSets=%u sizes=", i->flags, i->maxSets);
        for (uint32_t s = 0; s < i->poolSizeCount && i->pPoolSizes; ++s)
            t.add("%s%u:%u", s ? "," : "", unsigned(i->pPoolSizes[s].type),
                  i->pPoolSizes[s].descriptorCount);
        t.chain(i->pNext);
    }
    return record("vkCreateDescriptorPool", real_vkCreateDescriptorPool(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkAllocateDescriptorSets(VkDevice d,
    const VkDescriptorSetAllocateInfo *i, VkDescriptorSet *o)
{
    Text t;
    if (i) {
        t.add(" count=%u", i->descriptorSetCount);
        t.chain(i->pNext);
    }
    return record("vkAllocateDescriptorSets", real_vkAllocateDescriptorSets(d, i, o), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateDescriptorUpdateTemplate(VkDevice d,
    const VkDescriptorUpdateTemplateCreateInfo *i, const VkAllocationCallbacks *a,
    VkDescriptorUpdateTemplate *o)
{
    Text t;
    if (i) t.add(" entries=%u type=%u bindPoint=%u", i->descriptorUpdateEntryCount,
                 unsigned(i->templateType), unsigned(i->pipelineBindPoint));
    return record("vkCreateDescriptorUpdateTemplate",
                  real_vkCreateDescriptorUpdateTemplate(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateSampler(VkDevice d, const VkSamplerCreateInfo *i,
    const VkAllocationCallbacks *a, VkSampler *o)
{
    Text t;
    if (i) {
        t.add(" mag=%u min=%u mip=%u address=%u,%u,%u aniso=%u compare=%u border=%u unnormalized=%u",
              unsigned(i->magFilter), unsigned(i->minFilter), unsigned(i->mipmapMode),
              unsigned(i->addressModeU), unsigned(i->addressModeV), unsigned(i->addressModeW),
              i->anisotropyEnable, i->compareEnable, unsigned(i->borderColor),
              i->unnormalizedCoordinates);
        t.chain(i->pNext);
    }
    return record("vkCreateSampler", real_vkCreateSampler(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *i,
    const VkAllocationCallbacks *a, VkSemaphore *o)
{
    Text t;
    if (i) {
        t.add(" flags=0x%x", i->flags);
        t.chain(i->pNext);
    }
    return record("vkCreateSemaphore", real_vkCreateSemaphore(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateFence(VkDevice d, const VkFenceCreateInfo *i,
    const VkAllocationCallbacks *a, VkFence *o)
{
    Text t;
    if (i) {
        t.add(" flags=0x%x", i->flags);
        t.chain(i->pNext);
    }
    return record("vkCreateFence", real_vkCreateFence(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateEvent(VkDevice d, const VkEventCreateInfo *i,
    const VkAllocationCallbacks *a, VkEvent *o)
{
    Text t;
    if (i) t.add(" flags=0x%x", i->flags);
    return record("vkCreateEvent", real_vkCreateEvent(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo *i,
    const VkAllocationCallbacks *a, VkQueryPool *o)
{
    Text t;
    if (i) {
        t.add(" type=%u count=%u statistics=0x%x", unsigned(i->queryType), i->queryCount,
              i->pipelineStatistics);
        t.chain(i->pNext);
    }
    return record("vkCreateQueryPool", real_vkCreateQueryPool(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreatePipelineCache(VkDevice d,
    const VkPipelineCacheCreateInfo *i, const VkAllocationCallbacks *a, VkPipelineCache *o)
{
    Text t;
    if (i) t.add(" flags=0x%x initialData=%zu", i->flags, i->initialDataSize);
    return record("vkCreatePipelineCache", real_vkCreatePipelineCache(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateCommandPool(VkDevice d,
    const VkCommandPoolCreateInfo *i, const VkAllocationCallbacks *a, VkCommandPool *o)
{
    Text t;
    if (i) t.add(" flags=0x%x family=%u", i->flags, i->queueFamilyIndex);
    return record("vkCreateCommandPool", real_vkCreateCommandPool(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkAllocateCommandBuffers(VkDevice d,
    const VkCommandBufferAllocateInfo *i, VkCommandBuffer *o)
{
    Text t;
    if (i) t.add(" level=%u count=%u", unsigned(i->level), i->commandBufferCount);
    return record("vkAllocateCommandBuffers", real_vkAllocateCommandBuffers(d, i, o), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkBeginCommandBuffer(VkCommandBuffer c,
    const VkCommandBufferBeginInfo *i)
{
    Text t;
    if (i) {
        t.add(" flags=0x%x", i->flags);
        t.chain(i->pNext);
    }
    return record("vkBeginCommandBuffer", real_vkBeginCommandBuffer(c, i), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkEndCommandBuffer(VkCommandBuffer c)
{
    Text t;
    return record("vkEndCommandBuffer", real_vkEndCommandBuffer(c), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s,
    VkFence f)
{
    Text t;
    t.add(" submits=%u fence=%d", n, f != VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n && s && i < 4; ++i) {
        t.add(" s%u=cmd:%u,wait:%u,signal:%u", i, s[i].commandBufferCount,
              s[i].waitSemaphoreCount, s[i].signalSemaphoreCount);
        t.chain(s[i].pNext);
    }
    return record("vkQueueSubmit", real_vkQueueSubmit(q, n, s, f), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s,
    VkFence f)
{
    Text t;
    t.add(" submits=%u fence=%d", n, f != VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n && s && i < 4; ++i)
        t.add(" s%u=cmd:%u,wait:%u,signal:%u", i, s[i].commandBufferInfoCount,
              s[i].waitSemaphoreInfoCount, s[i].signalSemaphoreInfoCount);
    return record("vkQueueSubmit2", real_vkQueueSubmit2(q, n, s, f), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkQueueWaitIdle(VkQueue q)
{
    Text t;
    return record("vkQueueWaitIdle", real_vkQueueWaitIdle(q), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkDeviceWaitIdle(VkDevice d)
{
    Text t;
    return record("vkDeviceWaitIdle", real_vkDeviceWaitIdle(d), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f,
    VkBool32 all, uint64_t timeout)
{
    Text t;
    t.add(" count=%u all=%u timeout=%llu", n, all, static_cast<unsigned long long>(timeout));
    return record("vkWaitForFences", real_vkWaitForFences(d, n, f, all, timeout), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkWaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo *i,
    uint64_t timeout)
{
    Text t;
    if (i) t.add(" count=%u flags=0x%x timeout=%llu", i->semaphoreCount, i->flags,
                 static_cast<unsigned long long>(timeout));
    return record("vkWaitSemaphores", real_vkWaitSemaphores(d, i, timeout), t, false);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *i,
    const VkAllocationCallbacks *a, VkRenderPass *o)
{
    Text t;
    if (i) {
        t.add(" attachments=%u subpasses=%u dependencies=%u", i->attachmentCount,
              i->subpassCount, i->dependencyCount);
        t.chain(i->pNext);
    }
    return record("vkCreateRenderPass", real_vkCreateRenderPass(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2 *i,
    const VkAllocationCallbacks *a, VkRenderPass *o)
{
    Text t;
    if (i) {
        t.add(" attachments=%u subpasses=%u dependencies=%u", i->attachmentCount,
              i->subpassCount, i->dependencyCount);
        t.chain(i->pNext);
    }
    return record("vkCreateRenderPass2", real_vkCreateRenderPass2(d, i, a, o), t, true);
}

VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *i,
    const VkAllocationCallbacks *a, VkFramebuffer *o)
{
    Text t;
    if (i) {
        t.add(" flags=0x%x attachments=%u extent=%ux%ux%u", i->flags, i->attachmentCount,
              i->width, i->height, i->layers);
        t.chain(i->pNext);
    }
    return record("vkCreateFramebuffer", real_vkCreateFramebuffer(d, i, a, o), t, true);
}

/* ---- lookup ---- */

struct Hook {
    const char *name;
    PFN_vkVoidFunction wrapper;
    PFN_vkVoidFunction *real;
};

#define HOOK(name) {#name, reinterpret_cast<PFN_vkVoidFunction>(t_##name), \
                    reinterpret_cast<PFN_vkVoidFunction *>(&real_##name)}
#define HOOK_ALIAS(alias, name) {#alias, reinterpret_cast<PFN_vkVoidFunction>(t_##name), \
                                 reinterpret_cast<PFN_vkVoidFunction *>(&real_##name)}

const Hook g_hooks[] = {
    HOOK(vkEnumerateInstanceExtensionProperties),
    HOOK(vkEnumerateInstanceVersion),
    HOOK(vkCreateInstance),
    HOOK(vkEnumeratePhysicalDevices),
    HOOK(vkGetPhysicalDeviceProperties),
    HOOK(vkGetPhysicalDeviceProperties2),
    HOOK_ALIAS(vkGetPhysicalDeviceProperties2KHR, vkGetPhysicalDeviceProperties2),
    HOOK(vkGetPhysicalDeviceFeatures2),
    HOOK_ALIAS(vkGetPhysicalDeviceFeatures2KHR, vkGetPhysicalDeviceFeatures2),
    HOOK(vkEnumerateDeviceExtensionProperties),
    HOOK(vkCreateDevice),
    HOOK(vkCreateImage),
    HOOK(vkCreateImageView),
    HOOK(vkCreateBuffer),
    HOOK(vkCreateBufferView),
    HOOK(vkAllocateMemory),
    HOOK(vkBindImageMemory),
    HOOK(vkBindBufferMemory),
    HOOK(vkBindImageMemory2),
    HOOK_ALIAS(vkBindImageMemory2KHR, vkBindImageMemory2),
    HOOK(vkBindBufferMemory2),
    HOOK_ALIAS(vkBindBufferMemory2KHR, vkBindBufferMemory2),
    HOOK(vkMapMemory),
    HOOK(vkCreateShaderModule),
    HOOK(vkCreateGraphicsPipelines),
    HOOK(vkCreateComputePipelines),
    HOOK(vkCreatePipelineLayout),
    HOOK(vkCreateDescriptorSetLayout),
    HOOK(vkCreateDescriptorPool),
    HOOK(vkAllocateDescriptorSets),
    HOOK(vkCreateDescriptorUpdateTemplate),
    HOOK_ALIAS(vkCreateDescriptorUpdateTemplateKHR, vkCreateDescriptorUpdateTemplate),
    HOOK(vkCreateSampler),
    HOOK(vkCreateSemaphore),
    HOOK(vkCreateFence),
    HOOK(vkCreateEvent),
    HOOK(vkCreateQueryPool),
    HOOK(vkCreatePipelineCache),
    HOOK(vkCreateCommandPool),
    HOOK(vkAllocateCommandBuffers),
    HOOK(vkBeginCommandBuffer),
    HOOK(vkEndCommandBuffer),
    HOOK(vkQueueSubmit),
    HOOK(vkQueueSubmit2),
    HOOK_ALIAS(vkQueueSubmit2KHR, vkQueueSubmit2),
    HOOK(vkQueueWaitIdle),
    HOOK(vkDeviceWaitIdle),
    HOOK(vkWaitForFences),
    HOOK(vkWaitSemaphores),
    HOOK_ALIAS(vkWaitSemaphoresKHR, vkWaitSemaphores),
    HOOK(vkCreateRenderPass),
    HOOK(vkCreateRenderPass2),
    HOOK_ALIAS(vkCreateRenderPass2KHR, vkCreateRenderPass2),
    HOOK(vkCreateFramebuffer),
};

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL t_vkGetDeviceProcAddr(VkDevice device, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL t_vkGetInstanceProcAddr(VkInstance instance, const char *name);

PFN_vkVoidFunction resolve(const char *scope, PFN_vkVoidFunction found, const char *name)
{
    if (!name) return nullptr;
    if (!found) {
        unsigned index = g_missing++;
        if (index < 1024)
            dxvk_telemetry_emit("INFO", "DXVK_VK_MISSING scope=%s name=%s", scope, name);
        return nullptr;
    }
    if (!std::strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(t_vkGetInstanceProcAddr);
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) {
        real_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(found);
        return reinterpret_cast<PFN_vkVoidFunction>(t_vkGetDeviceProcAddr);
    }
    for (const Hook &hook : g_hooks) {
        if (!std::strcmp(hook.name, name)) {
            *hook.real = found;
            return hook.wrapper;
        }
    }
    return found;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL t_vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    return resolve(instance ? "instance" : "global", vkGetInstanceProcAddr(instance, name), name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL t_vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    PFN_vkVoidFunction found = real_vkGetDeviceProcAddr ? real_vkGetDeviceProcAddr(device, name)
                                                        : vkGetDeviceProcAddr(device, name);
    return resolve("device", found, name);
}

bool is_vulkan_library(const char *name)
{
    return name && (!std::strcmp(name, "libvulkan.so") || !std::strcmp(name, "libvulkan.so.1"));
}

} // namespace

extern "C" void *dlopen(const char *name, int flags)
{
    if (is_vulkan_library(name)) {
        dxvk_telemetry_stage("dxvk.load", "begin", "link=static d3d11+dxgi+ps5vk");
        dxvk_telemetry_emit("INFO", "DXVK_LOADER dlopen name=%s flags=0x%x resolved=static-ps5vk",
                            name, flags);
        return g_vulkan_handle;
    }
    dxvk_telemetry_emit("INFO", "DXVK_LOADER dlopen name=%s flags=0x%x resolved=none",
                        name ? name : "(null)", flags);
    return nullptr;
}

extern "C" void *dlsym(void *handle, const char *name)
{
    if (handle == g_vulkan_handle && name && !std::strcmp(name, "vkGetInstanceProcAddr")) {
        dxvk_telemetry_emit("INFO", "DXVK_LOADER dlsym name=vkGetInstanceProcAddr resolved=ps5vk entry=%p",
                            reinterpret_cast<void *>(&vkGetInstanceProcAddr));
        dxvk_telemetry_stage("dxvk.load", "ok", "vkGetInstanceProcAddr=ps5vk-static");
        return reinterpret_cast<void *>(t_vkGetInstanceProcAddr);
    }
    dxvk_telemetry_emit("INFO", "DXVK_LOADER dlsym name=%s resolved=none", name ? name : "(null)");
    return nullptr;
}

extern "C" int dlclose(void *handle)
{
    dxvk_telemetry_emit("INFO", "DXVK_LOADER dlclose vulkan=%d", handle == g_vulkan_handle);
    return handle == g_vulkan_handle ? 0 : -1;
}

extern "C" char *dlerror(void)
{
    static char message[] = "static payload: no loadable modules";
    return message;
}

/* libc's backtrace() symbolizes through dladdr; the static image has no
 * loader-visible symbol table, so report "not found". */
extern "C" int dladdr(const void *, void *)
{
    return 0;
}

extern "C" void dxvk_trace_summary(void)
{
    dxvk_telemetry_emit("INFO", "DXVK_VK_TRACE calls=%u refusals=%u missing_entry_points=%u",
                        g_calls.load(), g_refusals.load(), g_missing.load());
}
