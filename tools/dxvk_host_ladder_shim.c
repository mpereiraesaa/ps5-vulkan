/*
 * DIAGNOSTIC consumer-side Vulkan interposer for the DXVK host refusal ladder.
 *
 * Built only into the temporary directory of tools/run_dxvk_ps5vk_host_ladder.py
 * as libvulkan.so.1. It dlopen()s a host build of the ps5vk sources and
 * forwards every command to it. It is not part of ps5vk, never ships, and a
 * run through it is never evidence for an unmodified DXVK or a ps5vk
 * capability. Each bypass is off unless named in PS5VK_LADDER_BYPASS
 * (comma-separated), so every rung states exactly what was faked:
 *
 *   surface     advertise the surface extension names a host WSI driver asks
 *               for (VK_KHR_surface and the windowing-system ones) that ps5vk
 *               does not report, and strip exactly those again before
 *               vkCreateInstance reaches ps5vk. Surface commands ps5vk does
 *               not resolve are stubs that fail closed.
 *   api10       rewrite VkApplicationInfo::apiVersion 1.x -> 1.0 before
 *               vkCreateInstance reaches ps5vk.
 *   core_alias  enable VK_KHR_get_physical_device_properties2 and
 *               VK_KHR_device_group_creation on the instance, and resolve
 *               Vulkan 1.1/1.2 core command names to ps5vk's KHR commands.
 *   fake_features
 *               LIE: report the D3D11 baseline and feature-level 11_0 feature
 *               bits true in vkGetPhysicalDeviceFeatures2 and advertise the
 *               device extensions DXVK requires (VK_KHR_swapchain,
 *               VK_EXT_robustness2) plus VK_EXT_transform_feedback, so the run
 *               reaches vkCreateDevice and records the exact requested shape.
 *   strip_device
 *               drop from vkCreateDevice every extension, core feature bit and
 *               pNext structure ps5vk does not report, translating only
 *               Vulkan 1.2 timelineSemaphore/hostQueryReset to their KHR/EXT
 *               routes, so a device opens and DXVK's device bring-up is traced.
 *   emulate_mem_reqs
 *               answer the Vulkan 1.1 vkGet{Buffer,Image}MemoryRequirements2
 *               and 1.3 vkGetDevice{Buffer,Image}MemoryRequirements through
 *               ps5vk's 1.0 commands (the latter with a temporary object).
 *   drop_mutable
 *               remove VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT and the
 *               VkImageFormatListCreateInfo DXVK adds for UNORM/SRGB families
 *               from image format queries and vkCreateImage.
 *   drop_storage_texel
 *               remove VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT from vkCreateBuffer.
 *   fake_coherent
 *               LIE: report HOST_COHERENT on every HOST_VISIBLE memory type.
 *   format_properties3
 *               fill VkFormatProperties3 (the only format answer DXVK reads)
 *               from ps5vk's VkFormatProperties; the 32-bit flags are the low
 *               bits of the 64-bit ones, so this translates without adding.
 *
 * Independently of the bypasses the shim traces, one line each on stderr:
 *   LADDER_PROC_NULL  every name ps5vk resolved to NULL,
 *   LADDER_CALL       the shape and result of instance/device creation,
 *   LADDER_FAIL       any traced create/submit command that did not succeed.
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

enum { K_SURFACE = 1u, K_API10 = 2u, K_CORE_ALIAS = 4u, K_FAKE_FEATURES = 8u,
       K_STRIP_DEVICE = 16u, K_EMULATE_MEM_REQS = 32u, K_DROP_MUTABLE = 64u,
       K_DROP_STORAGE_TEXEL = 128u, K_FAKE_COHERENT = 256u,
       K_FORMAT_PROPERTIES3 = 512u };

static void *real_library;
static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr real_gdpa;
static unsigned knobs;
static int initialized;

static const char *const surface_names[] = {
    "VK_KHR_surface", "VK_KHR_xlib_surface", "VK_KHR_xcb_surface",
    "VK_KHR_wayland_surface",
};
#define SURFACE_COUNT (sizeof(surface_names) / sizeof(surface_names[0]))

static void init(void)
{
    if (initialized) return;
    initialized = 1;
    const char *path = getenv("PS5VK_LADDER_REAL");
    const char *bypass = getenv("PS5VK_LADDER_BYPASS");
    if (bypass) {
        char copy[256];
        snprintf(copy, sizeof(copy), "%s", bypass);
        for (char *save = NULL, *token = strtok_r(copy, ",", &save); token;
             token = strtok_r(NULL, ",", &save)) {
            if (!strcmp(token, "surface")) knobs |= K_SURFACE;
            else if (!strcmp(token, "api10")) knobs |= K_API10;
            else if (!strcmp(token, "core_alias")) knobs |= K_CORE_ALIAS;
            else if (!strcmp(token, "fake_features")) knobs |= K_FAKE_FEATURES;
            else if (!strcmp(token, "strip_device")) knobs |= K_STRIP_DEVICE;
            else if (!strcmp(token, "emulate_mem_reqs")) knobs |= K_EMULATE_MEM_REQS;
            else if (!strcmp(token, "drop_mutable")) knobs |= K_DROP_MUTABLE;
            else if (!strcmp(token, "drop_storage_texel")) knobs |= K_DROP_STORAGE_TEXEL;
            else if (!strcmp(token, "fake_coherent")) knobs |= K_FAKE_COHERENT;
            else if (!strcmp(token, "format_properties3")) knobs |= K_FORMAT_PROPERTIES3;
            else if (*token) {
                fprintf(stderr, "LADDER_ERROR unknown bypass %s\n", token);
                abort();
            }
        }
    }
    real_library = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND) : NULL;
    if (!real_library) {
        fprintf(stderr, "LADDER_ERROR cannot load ps5vk host library: %s\n",
                path ? dlerror() : "PS5VK_LADDER_REAL unset");
        abort();
    }
    real_gipa = (PFN_vkGetInstanceProcAddr)dlsym(real_library, "vkGetInstanceProcAddr");
    real_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(real_library, "vkGetDeviceProcAddr");
    if (!real_gipa || !real_gdpa) {
        fprintf(stderr, "LADDER_ERROR ps5vk host library lacks proc-address entry points\n");
        abort();
    }
    fprintf(stderr, "LADDER_SHIM bypass=%s\n", bypass && *bypass ? bypass : "none");
}

/* Whether ps5vk itself reports this instance extension. */
static int real_supports(const char *name)
{
    PFN_vkEnumerateInstanceExtensionProperties real =
        (PFN_vkEnumerateInstanceExtensionProperties)real_gipa(NULL,
            "vkEnumerateInstanceExtensionProperties");
    VkExtensionProperties props[32];
    uint32_t count = 32;
    if (!real || real(NULL, &count, props) < 0) return 0;
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(props[n].extensionName, name)) return 1;
    return 0;
}

static int is_surface_name(const char *name)
{
    for (size_t n = 0; n < SURFACE_COUNT; ++n)
        if (!strcmp(name, surface_names[n])) return 1;
    return 0;
}

/* Core names that ps5vk exposes only under a KHR/EXT suffix. */
static const char *core_alias(const char *name)
{
    static const char *const pairs[][2] = {
        {"vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceFeatures2KHR"},
        {"vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR"},
        {"vkGetPhysicalDeviceFormatProperties2", "vkGetPhysicalDeviceFormatProperties2KHR"},
        {"vkGetPhysicalDeviceImageFormatProperties2", "vkGetPhysicalDeviceImageFormatProperties2KHR"},
        {"vkGetPhysicalDeviceQueueFamilyProperties2", "vkGetPhysicalDeviceQueueFamilyProperties2KHR"},
        {"vkGetPhysicalDeviceMemoryProperties2", "vkGetPhysicalDeviceMemoryProperties2KHR"},
        {"vkGetPhysicalDeviceSparseImageFormatProperties2",
         "vkGetPhysicalDeviceSparseImageFormatProperties2KHR"},
        {"vkEnumeratePhysicalDeviceGroups", "vkEnumeratePhysicalDeviceGroupsKHR"},
        {"vkGetDeviceGroupPeerMemoryFeatures", "vkGetDeviceGroupPeerMemoryFeaturesKHR"},
        {"vkCmdDispatchBase", "vkCmdDispatchBaseKHR"},
        {"vkCmdSetDeviceMask", "vkCmdSetDeviceMaskKHR"},
        {"vkCreateRenderPass2", "vkCreateRenderPass2KHR"},
        {"vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR"},
        {"vkCmdNextSubpass2", "vkCmdNextSubpass2KHR"},
        {"vkCmdEndRenderPass2", "vkCmdEndRenderPass2KHR"},
        {"vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR"},
        {"vkWaitSemaphores", "vkWaitSemaphoresKHR"},
        {"vkSignalSemaphore", "vkSignalSemaphoreKHR"},
        {"vkGetBufferDeviceAddress", "vkGetBufferDeviceAddressKHR"},
        {"vkGetBufferOpaqueCaptureAddress", "vkGetBufferOpaqueCaptureAddressKHR"},
        {"vkGetDeviceMemoryOpaqueCaptureAddress", "vkGetDeviceMemoryOpaqueCaptureAddressKHR"},
    };
    for (size_t n = 0; n < sizeof(pairs) / sizeof(pairs[0]); ++n)
        if (!strcmp(name, pairs[n][0])) return pairs[n][1];
    return NULL;
}

/* ---- fail-closed surface stubs (bypass "surface") ------------------------ */
static VKAPI_ATTR void VKAPI_CALL stub_destroy_surface(VkInstance i, VkSurfaceKHR s,
    const VkAllocationCallbacks *a)
{ (void)i; (void)a; if (s) fprintf(stderr, "LADDER_STUB vkDestroySurfaceKHR\n"); }
static VKAPI_ATTR VkResult VKAPI_CALL stub_surface_unsupported(void)
{
    fprintf(stderr, "LADDER_STUB surface command called: no WSI behind this shim\n");
    return VK_ERROR_SURFACE_LOST_KHR;
}

/* ---- traced instance commands -------------------------------------------- */
static VKAPI_ATTR VkResult VKAPI_CALL shim_enumerate_instance_extensions(
    const char *layer, uint32_t *count, VkExtensionProperties *out)
{
    PFN_vkEnumerateInstanceExtensionProperties real =
        (PFN_vkEnumerateInstanceExtensionProperties)real_gipa(NULL,
            "vkEnumerateInstanceExtensionProperties");
    if (layer || !(knobs & K_SURFACE) || !count) return real(layer, count, out);
    uint32_t base = 0;
    VkResult r = real(NULL, &base, NULL);
    if (r != VK_SUCCESS) return r;
    const uint32_t total = base + (uint32_t)SURFACE_COUNT;
    VkExtensionProperties *all = calloc(total, sizeof(*all));
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t have = base;
    r = real(NULL, &have, all);
    if (r != VK_SUCCESS) { free(all); return r; }
    for (size_t n = 0; n < SURFACE_COUNT; ++n) {
        int present = 0;
        for (uint32_t k = 0; k < base; ++k)
            present |= !strcmp(all[k].extensionName, surface_names[n]);
        if (present) continue;
        snprintf(all[have].extensionName, VK_MAX_EXTENSION_NAME_SIZE, "%s", surface_names[n]);
        all[have++].specVersion = 1;
    }
    if (!out) { *count = have; free(all); return VK_SUCCESS; }
    const uint32_t written = *count < have ? *count : have;
    memcpy(out, all, written * sizeof(*out));
    free(all);
    *count = written;
    return written < have ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_create_instance(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *out)
{
    PFN_vkCreateInstance real = (PFN_vkCreateInstance)real_gipa(NULL, "vkCreateInstance");
    const uint32_t api = info && info->pApplicationInfo ? info->pApplicationInfo->apiVersion : 0;
    fprintf(stderr, "LADDER_CALL vkCreateInstance apiVersion=%u.%u.%u layers=%u pNext=%s extensions=",
            VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
            info ? info->enabledLayerCount : 0, info && info->pNext ? "yes" : "no");
    for (uint32_t n = 0; info && n < info->enabledExtensionCount; ++n)
        fprintf(stderr, "%s%s", n ? "," : "", info->ppEnabledExtensionNames[n]);
    fprintf(stderr, "\n");
    if (!info) return real(info, allocator, out);
    VkInstanceCreateInfo copy = *info;
    VkApplicationInfo app;
    const char *names[64];
    uint32_t kept = 0;
    for (uint32_t n = 0; n < info->enabledExtensionCount && kept < 60; ++n) {
        const char *name = info->ppEnabledExtensionNames[n];
        if ((knobs & K_SURFACE) && is_surface_name(name) && !real_supports(name)) {
            fprintf(stderr, "LADDER_STRIP instance_extension=%s\n", name);
            continue;
        }
        if ((knobs & K_CORE_ALIAS) &&
            (!strcmp(name, "VK_KHR_get_physical_device_properties2") ||
             !strcmp(name, "VK_KHR_device_group_creation"))) continue;
        names[kept++] = name;
    }
    if (knobs & K_CORE_ALIAS) {
        names[kept++] = "VK_KHR_get_physical_device_properties2";
        names[kept++] = "VK_KHR_device_group_creation";
    }
    copy.enabledExtensionCount = kept;
    copy.ppEnabledExtensionNames = kept ? names : NULL;
    if ((knobs & K_API10) && info->pApplicationInfo) {
        app = *info->pApplicationInfo;
        if (VK_API_VERSION_MAJOR(app.apiVersion) == 1) app.apiVersion = VK_API_VERSION_1_0;
        copy.pApplicationInfo = &app;
    }
    VkResult r = real(&copy, allocator, out);
    fprintf(stderr, "LADDER_CALL vkCreateInstance forwarded_apiVersion=%u.%u result=%d\n",
            copy.pApplicationInfo ? VK_API_VERSION_MAJOR(copy.pApplicationInfo->apiVersion) : 0,
            copy.pApplicationInfo ? VK_API_VERSION_MINOR(copy.pApplicationInfo->apiVersion) : 0,
            (int)r);
    return r;
}

/* ---- D3D11 feature requirements (d3d11_device.cpp GetDeviceFeatures and
 * d3d11_features.cpp GetMaxFeatureLevel of the pinned DXVK) ------------------ */
#define CORE(m) {#m, offsetof(VkPhysicalDeviceFeatures, m)}
static const struct { const char *name; size_t offset; } d3d11_core[] = {
    CORE(depthBiasClamp), CORE(depthClamp), CORE(dualSrcBlend), CORE(fillModeNonSolid),
    CORE(fullDrawIndexUint32), CORE(geometryShader), CORE(imageCubeArray),
    CORE(independentBlend), CORE(multiViewport), CORE(occlusionQueryPrecise),
    CORE(sampleRateShading), CORE(shaderClipDistance), CORE(shaderCullDistance),
    CORE(shaderImageGatherExtended), CORE(textureCompressionBC),
    /* feature level 11_0 */
    CORE(drawIndirectFirstInstance), CORE(fragmentStoresAndAtomics),
    CORE(multiDrawIndirect), CORE(tessellationShader),
};
#undef CORE
static const char *const fake_device_extensions[] = {
    "VK_KHR_swapchain", "VK_EXT_robustness2", "VK_EXT_transform_feedback",
};
#define FAKE_EXTENSION_COUNT (sizeof(fake_device_extensions) / sizeof(fake_device_extensions[0]))

/* Appends a missing bit to one of two lists and, under fake_features, sets it. */
struct missing { char text[1024]; size_t used; };
static void require(struct missing *list, const char *name, VkBool32 *bit)
{
    if (*bit) return;
    int n = snprintf(list->text + list->used, sizeof(list->text) - list->used, "%s%s",
                     list->used ? "," : "", name);
    if (n > 0 && list->used + (size_t)n < sizeof(list->text)) list->used += (size_t)n;
    if (knobs & K_FAKE_FEATURES) *bit = VK_TRUE;
}

static PFN_vkGetPhysicalDeviceFeatures2 real_features2;
static VKAPI_ATTR void VKAPI_CALL shim_features2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *out)
{
    real_features2(p, out);
    if (!out) return;
    /* checked: D3D11Device::GetMaxFeatureLevel refuses FL11_0 without it.
     * forced: DxvkAdapter::createDevice enables it without checking, so
     * vkCreateDevice must accept it. */
    struct missing checked = {{0}, 0}, forced = {{0}, 0};
    for (size_t n = 0; n < sizeof(d3d11_core) / sizeof(d3d11_core[0]); ++n)
        require(&checked, d3d11_core[n].name,
                (VkBool32 *)((unsigned char *)&out->features + d3d11_core[n].offset));
    require(&forced, "robustBufferAccess", &out->features.robustBufferAccess);
    int transform_feedback = 0;
    for (VkBaseOutStructure *s = out->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            require(&forced, "vk11.shaderDrawParameters",
                    &((VkPhysicalDeviceVulkan11Features *)s)->shaderDrawParameters);
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            VkPhysicalDeviceVulkan12Features *f = (VkPhysicalDeviceVulkan12Features *)s;
            require(&checked, "vk12.samplerMirrorClampToEdge", &f->samplerMirrorClampToEdge);
            require(&forced, "vk12.vulkanMemoryModel", &f->vulkanMemoryModel);
            require(&forced, "vk12.hostQueryReset", &f->hostQueryReset);
            require(&forced, "vk12.timelineSemaphore", &f->timelineSemaphore);
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            VkPhysicalDeviceVulkan13Features *f = (VkPhysicalDeviceVulkan13Features *)s;
            require(&checked, "vk13.shaderDemoteToHelperInvocation",
                    &f->shaderDemoteToHelperInvocation);
            require(&forced, "vk13.synchronization2", &f->synchronization2);
            require(&forced, "vk13.dynamicRendering", &f->dynamicRendering);
            require(&forced, "vk13.maintenance4", &f->maintenance4);
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT) {
            VkPhysicalDeviceTransformFeedbackFeaturesEXT *f =
                (VkPhysicalDeviceTransformFeedbackFeaturesEXT *)s;
            transform_feedback = 1;
            require(&checked, "transformFeedback", &f->transformFeedback);
            require(&forced, "geometryStreams", &f->geometryStreams);
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            VkPhysicalDeviceRobustness2FeaturesEXT *f = (VkPhysicalDeviceRobustness2FeaturesEXT *)s;
            require(&forced, "robustBufferAccess2", &f->robustBufferAccess2);
            require(&forced, "nullDescriptor", &f->nullDescriptor);
        }
    }
    VkBool32 absent = VK_FALSE;
    if (!transform_feedback) require(&checked, "transformFeedback(VK_EXT_transform_feedback)", &absent);
    fprintf(stderr, "LADDER_FEATURES checked_missing=%s\n", checked.used ? checked.text : "none");
    fprintf(stderr, "LADDER_FEATURES forced_missing=%s\n", forced.used ? forced.text : "none");
}

static PFN_vkGetPhysicalDeviceImageFormatProperties2 real_image_format2;
static VKAPI_ATTR VkResult VKAPI_CALL shim_image_format2(VkPhysicalDevice p,
    const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out)
{
    VkPhysicalDeviceImageFormatInfo2 copy;
    if ((knobs & K_DROP_MUTABLE) && info && (info->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) {
        copy = *info;
        copy.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        info = &copy;
    }
    VkResult r = real_image_format2(p, info, out);
    if (r != VK_SUCCESS && info) {
        fprintf(stderr, "LADDER_FAIL vkGetPhysicalDeviceImageFormatProperties2 format=%d type=%d "
                "tiling=%d usage=0x%x flags=0x%x pNext_sTypes=", (int)info->format,
                (int)info->type, (int)info->tiling, info->usage, info->flags);
        for (const VkBaseInStructure *s = info->pNext; s; s = s->pNext)
            fprintf(stderr, "%d%s", (int)s->sType, s->pNext ? "," : "");
        fprintf(stderr, " out_pNext_sTypes=");
        for (const VkBaseOutStructure *s = out ? out->pNext : NULL; s; s = s->pNext)
            fprintf(stderr, "%d%s", (int)s->sType, s->pNext ? "," : "");
        fprintf(stderr, " result=%d\n", (int)r);
    }
    return r;
}

static PFN_vkGetPhysicalDeviceMemoryProperties real_memory_properties;
static VKAPI_ATTR void VKAPI_CALL shim_memory_properties(VkPhysicalDevice p,
    VkPhysicalDeviceMemoryProperties *out)
{
    real_memory_properties(p, out);
    for (uint32_t n = 0; out && n < out->memoryTypeCount; ++n) {
        const VkMemoryPropertyFlags flags = out->memoryTypes[n].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            fprintf(stderr, "LADDER_MEMORY type=%u flags=0x%x lacks HOST_COHERENT%s\n", n, flags,
                    (knobs & K_FAKE_COHERENT) ? " (faked)" : "");
            if (knobs & K_FAKE_COHERENT)
                out->memoryTypes[n].propertyFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
    }
}

static PFN_vkGetPhysicalDeviceFormatProperties2 real_format2;
static int format3_logged;
static VKAPI_ATTR void VKAPI_CALL shim_format2(VkPhysicalDevice p, VkFormat format,
    VkFormatProperties2 *out)
{
    real_format2(p, format, out);
    for (VkBaseOutStructure *s = out ? out->pNext : NULL; s; s = s->pNext) {
        if (s->sType != VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) continue;
        VkFormatProperties3 *f = (VkFormatProperties3 *)s;
        if (!format3_logged++)
            fprintf(stderr, "LADDER_FORMAT3 first query format=%d reported optimal=0x%llx "
                    "(legacy optimal=0x%x)%s\n", (int)format,
                    (unsigned long long)f->optimalTilingFeatures,
                    out->formatProperties.optimalTilingFeatures,
                    (knobs & K_FORMAT_PROPERTIES3) ? " translated" : "");
        if (knobs & K_FORMAT_PROPERTIES3) {
            f->optimalTilingFeatures = out->formatProperties.optimalTilingFeatures;
            f->linearTilingFeatures = out->formatProperties.linearTilingFeatures;
            f->bufferFeatures = out->formatProperties.bufferFeatures;
        }
    }
}

static PFN_vkEnumerateDeviceExtensionProperties real_enum_device_ext;
static VKAPI_ATTR VkResult VKAPI_CALL shim_enum_device_ext(VkPhysicalDevice p, const char *layer,
    uint32_t *count, VkExtensionProperties *out)
{
    if (layer || !count || !(knobs & K_FAKE_FEATURES))
        return real_enum_device_ext(p, layer, count, out);
    uint32_t base = 0;
    VkResult r = real_enum_device_ext(p, NULL, &base, NULL);
    if (r != VK_SUCCESS) return r;
    const uint32_t total = base + (uint32_t)FAKE_EXTENSION_COUNT;
    if (!out) { *count = total; return VK_SUCCESS; }
    VkExtensionProperties *all = calloc(total, sizeof(*all));
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t got = base;
    r = real_enum_device_ext(p, NULL, &got, all);
    if (r != VK_SUCCESS) { free(all); return r; }
    for (size_t n = 0; n < FAKE_EXTENSION_COUNT; ++n) {
        snprintf(all[got + n].extensionName, VK_MAX_EXTENSION_NAME_SIZE, "%s",
                 fake_device_extensions[n]);
        all[got + n].specVersion = 1;
    }
    const uint32_t have = got + (uint32_t)FAKE_EXTENSION_COUNT;
    const uint32_t written = *count < have ? *count : have;
    memcpy(out, all, written * sizeof(*out));
    free(all);
    *count = written;
    return written < have ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- traced device creation ---------------------------------------------- */
static PFN_vkCreateDevice real_create_device;
static PFN_vkGetPhysicalDeviceFeatures real_get_features;

static void trace_core_features(const char *label, const VkPhysicalDeviceFeatures *f,
                                const VkPhysicalDeviceFeatures *supported)
{
    static const char *const names[] = {
        "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend",
        "geometryShader", "tessellationShader", "sampleRateShading", "dualSrcBlend",
        "logicOp", "multiDrawIndirect", "drawIndirectFirstInstance", "depthClamp",
        "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines", "largePoints",
        "alphaToOne", "multiViewport", "samplerAnisotropy", "textureCompressionETC2",
        "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise",
        "pipelineStatisticsQuery", "vertexPipelineStoresAndAtomics",
        "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize",
        "shaderImageGatherExtended", "shaderStorageImageExtendedFormats",
        "shaderStorageImageMultisample", "shaderStorageImageReadWithoutFormat",
        "shaderStorageImageWriteWithoutFormat", "shaderUniformBufferArrayDynamicIndexing",
        "shaderSampledImageArrayDynamicIndexing", "shaderStorageBufferArrayDynamicIndexing",
        "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance", "shaderCullDistance",
        "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency",
        "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer",
        "sparseResidencyImage2D", "sparseResidencyImage3D", "sparseResidency2Samples",
        "sparseResidency4Samples", "sparseResidency8Samples", "sparseResidency16Samples",
        "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries",
    };
    const VkBool32 *bits = (const VkBool32 *)f;
    const VkBool32 *have = (const VkBool32 *)supported;
    fprintf(stderr, "LADDER_CALL %s core_features=", label);
    int first = 1;
    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n)
        if (bits[n]) {
            fprintf(stderr, "%s%s%s", first ? "" : ",", names[n], have && !have[n] ? "(UNSUPPORTED)" : "");
            first = 0;
        }
    fprintf(stderr, "\n");
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_create_device(VkPhysicalDevice p,
    const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *out)
{
    fprintf(stderr, "LADDER_CALL vkCreateDevice queues=%u extensions=",
            info ? info->queueCreateInfoCount : 0);
    for (uint32_t n = 0; info && n < info->enabledExtensionCount; ++n)
        fprintf(stderr, "%s%s", n ? "," : "", info->ppEnabledExtensionNames[n]);
    fprintf(stderr, "\nLADDER_CALL vkCreateDevice pNext_sTypes=");
    const VkPhysicalDeviceFeatures *core = info ? info->pEnabledFeatures : NULL;
    for (const VkBaseInStructure *s = info ? info->pNext : NULL; s; s = s->pNext) {
        fprintf(stderr, "%d%s", (int)s->sType, s->pNext ? "," : "");
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            core = &((const VkPhysicalDeviceFeatures2 *)s)->features;
    }
    fprintf(stderr, "\n");
    for (uint32_t n = 0; info && n < info->queueCreateInfoCount; ++n)
        fprintf(stderr, "LADDER_CALL vkCreateDevice queue family=%u count=%u flags=0x%x pNext=%s\n",
                info->pQueueCreateInfos[n].queueFamilyIndex, info->pQueueCreateInfos[n].queueCount,
                info->pQueueCreateInfos[n].flags, info->pQueueCreateInfos[n].pNext ? "yes" : "no");
    if (core) {
        VkPhysicalDeviceFeatures supported;
        memset(&supported, 0, sizeof(supported));
        if (real_get_features) real_get_features(p, &supported);
        trace_core_features("vkCreateDevice", core, &supported);
    }
    if (!(knobs & K_STRIP_DEVICE) || !info) {
        VkResult r = real_create_device(p, info, allocator, out);
        fprintf(stderr, "LADDER_CALL vkCreateDevice result=%d\n", (int)r);
        return r;
    }
    /* Sanitize: keep only what ps5vk reports, translate two 1.2 bits. */
    VkExtensionProperties have[64];
    uint32_t have_count = 64;
    real_enum_device_ext(p, NULL, &have_count, have);
    const char *names[64];
    uint32_t kept = 0;
    for (uint32_t n = 0; n < info->enabledExtensionCount && kept < 60; ++n) {
        int found = 0;
        for (uint32_t k = 0; k < have_count; ++k)
            found |= !strcmp(have[k].extensionName, info->ppEnabledExtensionNames[n]);
        if (found) names[kept++] = info->ppEnabledExtensionNames[n];
        else fprintf(stderr, "LADDER_STRIP extension=%s\n", info->ppEnabledExtensionNames[n]);
    }
    VkPhysicalDeviceFeatures supported, enabled;
    memset(&supported, 0, sizeof(supported));
    real_get_features(p, &supported);
    memset(&enabled, 0, sizeof(enabled));
    if (core) {
        const VkBool32 *want = (const VkBool32 *)core, *can = (const VkBool32 *)&supported;
        VkBool32 *set = (VkBool32 *)&enabled;
        for (size_t n = 0; n < sizeof(enabled) / sizeof(VkBool32); ++n) {
            set[n] = want[n] && can[n];
            if (want[n] && !can[n]) fprintf(stderr, "LADDER_STRIP core_feature_index=%zu\n", n);
        }
    }
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, NULL, VK_TRUE};
    VkPhysicalDeviceHostQueryResetFeatures query_reset = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, NULL, VK_TRUE};
    const void *chain = NULL;
    for (const VkBaseInStructure *s = info->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            const VkPhysicalDeviceVulkan12Features *f = (const VkPhysicalDeviceVulkan12Features *)s;
            for (uint32_t k = 0; k < have_count; ++k) {
                if (f->timelineSemaphore && !strcmp(have[k].extensionName, "VK_KHR_timeline_semaphore")) {
                    names[kept++] = "VK_KHR_timeline_semaphore";
                    timeline.pNext = (void *)chain; chain = &timeline;
                    fprintf(stderr, "LADDER_TRANSLATE vk12.timelineSemaphore -> VK_KHR_timeline_semaphore\n");
                }
                if (f->hostQueryReset && !strcmp(have[k].extensionName, "VK_EXT_host_query_reset")) {
                    names[kept++] = "VK_EXT_host_query_reset";
                    query_reset.pNext = (void *)chain; chain = &query_reset;
                    fprintf(stderr, "LADDER_TRANSLATE vk12.hostQueryReset -> VK_EXT_host_query_reset\n");
                }
            }
        }
        if (s->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            fprintf(stderr, "LADDER_STRIP pNext_sType=%d\n", (int)s->sType);
    }
    VkDeviceCreateInfo copy = *info;
    copy.pNext = chain;
    copy.enabledExtensionCount = kept;
    copy.ppEnabledExtensionNames = kept ? names : NULL;
    copy.pEnabledFeatures = &enabled;
    VkResult r = real_create_device(p, &copy, allocator, out);
    fprintf(stderr, "LADDER_CALL vkCreateDevice stripped result=%d\n", (int)r);
    return r;
}

/* ---- traced failures of the commands DXVK runs after device creation ----- */
#define TRACED_RESULT(fn, params, args)                                         \
    static PFN_##fn real_##fn;                                                   \
    static VKAPI_ATTR VkResult VKAPI_CALL shim_##fn params {                     \
        VkResult r = real_##fn args;                                             \
        if (r != VK_SUCCESS)                                                     \
            fprintf(stderr, "LADDER_FAIL " #fn " result=%d\n", (int)r);          \
        return r;                                                                \
    }
TRACED_RESULT(vkAllocateMemory, (VkDevice d, const VkMemoryAllocateInfo *i,
    const VkAllocationCallbacks *a, VkDeviceMemory *o), (d, i, a, o))
static PFN_vkCreateBuffer real_vkCreateBuffer;
static VKAPI_ATTR VkResult VKAPI_CALL shim_vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *i,
    const VkAllocationCallbacks *a, VkBuffer *o)
{
    VkBufferCreateInfo copy;
    if ((knobs & K_DROP_STORAGE_TEXEL) && i &&
        (i->usage & VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) {
        copy = *i;
        copy.usage &= ~(VkBufferUsageFlags)VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
        fprintf(stderr, "LADDER_DROP_STORAGE_TEXEL vkCreateBuffer usage=0x%x\n", i->usage);
        i = &copy;
    }
    VkResult r = real_vkCreateBuffer(d, i, a, o);
    if (r != VK_SUCCESS && i)
        fprintf(stderr, "LADDER_FAIL vkCreateBuffer size=%llu usage=0x%x flags=0x%x sharing=%d "
                "pNext=%s result=%d\n", (unsigned long long)i->size, i->usage, i->flags,
                (int)i->sharingMode, i->pNext ? "yes" : "no", (int)r);
    return r;
}
static PFN_vkCreateImage real_vkCreateImage;
static VKAPI_ATTR VkResult VKAPI_CALL shim_vkCreateImage(VkDevice d, const VkImageCreateInfo *i,
    const VkAllocationCallbacks *a, VkImage *o)
{
    VkImageCreateInfo copy;
    if ((knobs & K_DROP_MUTABLE) && i && (i->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) {
        copy = *i;
        copy.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        /* DXVK's only chained struct here is the format list; drop the chain. */
        copy.pNext = NULL;
        fprintf(stderr, "LADDER_DROP_MUTABLE vkCreateImage format=%d\n", (int)i->format);
        i = &copy;
    }
    VkResult r = real_vkCreateImage(d, i, a, o);
    if (r != VK_SUCCESS && i) {
        fprintf(stderr, "LADDER_FAIL vkCreateImage format=%d type=%d extent=%ux%ux%u mips=%u "
                "layers=%u samples=%d tiling=%d usage=0x%x flags=0x%x pNext_sTypes=",
                (int)i->format, (int)i->imageType, i->extent.width, i->extent.height,
                i->extent.depth, i->mipLevels, i->arrayLayers, (int)i->samples,
                (int)i->tiling, i->usage, i->flags);
        for (const VkBaseInStructure *s = i->pNext; s; s = s->pNext)
            fprintf(stderr, "%d%s", (int)s->sType, s->pNext ? "," : "");
        fprintf(stderr, " result=%d\n", (int)r);
    }
    return r;
}
TRACED_RESULT(vkCreateImageView, (VkDevice d, const VkImageViewCreateInfo *i,
    const VkAllocationCallbacks *a, VkImageView *o), (d, i, a, o))
TRACED_RESULT(vkCreateSampler, (VkDevice d, const VkSamplerCreateInfo *i,
    const VkAllocationCallbacks *a, VkSampler *o), (d, i, a, o))
TRACED_RESULT(vkCreateSemaphore, (VkDevice d, const VkSemaphoreCreateInfo *i,
    const VkAllocationCallbacks *a, VkSemaphore *o), (d, i, a, o))
TRACED_RESULT(vkCreateFence, (VkDevice d, const VkFenceCreateInfo *i,
    const VkAllocationCallbacks *a, VkFence *o), (d, i, a, o))
TRACED_RESULT(vkCreateCommandPool, (VkDevice d, const VkCommandPoolCreateInfo *i,
    const VkAllocationCallbacks *a, VkCommandPool *o), (d, i, a, o))
TRACED_RESULT(vkCreateDescriptorSetLayout, (VkDevice d, const VkDescriptorSetLayoutCreateInfo *i,
    const VkAllocationCallbacks *a, VkDescriptorSetLayout *o), (d, i, a, o))
TRACED_RESULT(vkCreatePipelineLayout, (VkDevice d, const VkPipelineLayoutCreateInfo *i,
    const VkAllocationCallbacks *a, VkPipelineLayout *o), (d, i, a, o))
TRACED_RESULT(vkCreateShaderModule, (VkDevice d, const VkShaderModuleCreateInfo *i,
    const VkAllocationCallbacks *a, VkShaderModule *o), (d, i, a, o))
TRACED_RESULT(vkCreateQueryPool, (VkDevice d, const VkQueryPoolCreateInfo *i,
    const VkAllocationCallbacks *a, VkQueryPool *o), (d, i, a, o))
TRACED_RESULT(vkCreatePipelineCache, (VkDevice d, const VkPipelineCacheCreateInfo *i,
    const VkAllocationCallbacks *a, VkPipelineCache *o), (d, i, a, o))
TRACED_RESULT(vkCreateGraphicsPipelines, (VkDevice d, VkPipelineCache c, uint32_t n,
    const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o),
    (d, c, n, i, a, o))
TRACED_RESULT(vkCreateComputePipelines, (VkDevice d, VkPipelineCache c, uint32_t n,
    const VkComputePipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o),
    (d, c, n, i, a, o))
TRACED_RESULT(vkBeginCommandBuffer, (VkCommandBuffer b, const VkCommandBufferBeginInfo *i), (b, i))
TRACED_RESULT(vkEndCommandBuffer, (VkCommandBuffer b), (b))
TRACED_RESULT(vkQueueSubmit, (VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f), (q, n, s, f))
TRACED_RESULT(vkBindBufferMemory, (VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o),
    (d, b, m, o))
TRACED_RESULT(vkBindImageMemory, (VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize o),
    (d, i, m, o))
TRACED_RESULT(vkMapMemory, (VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s,
    VkMemoryMapFlags f, void **p), (d, m, o, s, f, p))

static PFN_vkVoidFunction traced(const char *name, PFN_vkVoidFunction real)
{
#define TRACE(fn) if (!strcmp(name, #fn)) { real_##fn = (PFN_##fn)real; \
        return (PFN_vkVoidFunction)shim_##fn; }
    if (!real) return NULL;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2") ||
        !strcmp(name, "vkGetPhysicalDeviceFeatures2KHR")) {
        real_features2 = (PFN_vkGetPhysicalDeviceFeatures2)real;
        return (PFN_vkVoidFunction)shim_features2;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2") ||
        !strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR")) {
        real_image_format2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)real;
        return (PFN_vkVoidFunction)shim_image_format2;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2") ||
        !strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR")) {
        real_format2 = (PFN_vkGetPhysicalDeviceFormatProperties2)real;
        return (PFN_vkVoidFunction)shim_format2;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties")) {
        real_memory_properties = (PFN_vkGetPhysicalDeviceMemoryProperties)real;
        return (PFN_vkVoidFunction)shim_memory_properties;
    }
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) {
        real_enum_device_ext = (PFN_vkEnumerateDeviceExtensionProperties)real;
        return (PFN_vkVoidFunction)shim_enum_device_ext;
    }
    TRACE(vkAllocateMemory) TRACE(vkCreateBuffer) TRACE(vkCreateImage)
    TRACE(vkCreateImageView) TRACE(vkCreateSampler) TRACE(vkCreateSemaphore)
    TRACE(vkCreateFence) TRACE(vkCreateCommandPool) TRACE(vkCreateDescriptorSetLayout)
    TRACE(vkCreatePipelineLayout) TRACE(vkCreateShaderModule) TRACE(vkCreateQueryPool)
    TRACE(vkCreatePipelineCache) TRACE(vkCreateGraphicsPipelines)
    TRACE(vkCreateComputePipelines) TRACE(vkBeginCommandBuffer) TRACE(vkEndCommandBuffer)
    TRACE(vkQueueSubmit) TRACE(vkBindBufferMemory) TRACE(vkBindImageMemory) TRACE(vkMapMemory)
#undef TRACE
    return real;
}

/* ---- emulate_mem_reqs ----------------------------------------------------- */
static VkDevice emulation_device;
#define REAL(fn) ((PFN_##fn)real_gdpa(emulation_device, #fn))
static void fill_requirements(VkMemoryRequirements2 *out, const VkMemoryRequirements *in)
{
    out->memoryRequirements = *in;
    for (VkBaseOutStructure *s = out->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            ((VkMemoryDedicatedRequirements *)s)->prefersDedicatedAllocation = VK_FALSE;
            ((VkMemoryDedicatedRequirements *)s)->requiresDedicatedAllocation = VK_FALSE;
        }
}
static VKAPI_ATTR void VKAPI_CALL emu_buffer_reqs2(VkDevice d,
    const VkBufferMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *out)
{
    VkMemoryRequirements r = {0};
    REAL(vkGetBufferMemoryRequirements)(d, info->buffer, &r);
    fill_requirements(out, &r);
}
static VKAPI_ATTR void VKAPI_CALL emu_image_reqs2(VkDevice d,
    const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *out)
{
    VkMemoryRequirements r = {0};
    REAL(vkGetImageMemoryRequirements)(d, info->image, &r);
    fill_requirements(out, &r);
}
static VKAPI_ATTR void VKAPI_CALL emu_device_buffer_reqs(VkDevice d,
    const VkDeviceBufferMemoryRequirements *info, VkMemoryRequirements2 *out)
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkMemoryRequirements r = {0};
    VkResult result = REAL(vkCreateBuffer)(d, info->pCreateInfo, NULL, &buffer);
    if (result == VK_SUCCESS) {
        REAL(vkGetBufferMemoryRequirements)(d, buffer, &r);
        REAL(vkDestroyBuffer)(d, buffer, NULL);
    } else {
        fprintf(stderr, "LADDER_FAIL vkGetDeviceBufferMemoryRequirements(emulated vkCreateBuffer "
                "usage=0x%x flags=0x%x size=%llu pNext=%s) result=%d\n",
                info->pCreateInfo->usage, info->pCreateInfo->flags,
                (unsigned long long)info->pCreateInfo->size,
                info->pCreateInfo->pNext ? "yes" : "no", (int)result);
    }
    fill_requirements(out, &r);
}
static VKAPI_ATTR void VKAPI_CALL emu_device_image_reqs(VkDevice d,
    const VkDeviceImageMemoryRequirements *info, VkMemoryRequirements2 *out)
{
    VkImage image = VK_NULL_HANDLE;
    VkMemoryRequirements r = {0};
    VkResult result = REAL(vkCreateImage)(d, info->pCreateInfo, NULL, &image);
    if (result == VK_SUCCESS) {
        REAL(vkGetImageMemoryRequirements)(d, image, &r);
        REAL(vkDestroyImage)(d, image, NULL);
    } else {
        fprintf(stderr, "LADDER_FAIL vkGetDeviceImageMemoryRequirements(emulated vkCreateImage "
                "format=%d usage=0x%x flags=0x%x tiling=%d pNext=%s) result=%d\n",
                (int)info->pCreateInfo->format, info->pCreateInfo->usage,
                info->pCreateInfo->flags, (int)info->pCreateInfo->tiling,
                info->pCreateInfo->pNext ? "yes" : "no", (int)result);
    }
    fill_requirements(out, &r);
}
#undef REAL

static PFN_vkVoidFunction emulated(VkDevice device, const char *name)
{
    if (!(knobs & K_EMULATE_MEM_REQS) || !device) return NULL;
    emulation_device = device;
    if (!strcmp(name, "vkGetBufferMemoryRequirements2")) return (PFN_vkVoidFunction)emu_buffer_reqs2;
    if (!strcmp(name, "vkGetImageMemoryRequirements2")) return (PFN_vkVoidFunction)emu_image_reqs2;
    if (!strcmp(name, "vkGetDeviceBufferMemoryRequirements"))
        return (PFN_vkVoidFunction)emu_device_buffer_reqs;
    if (!strcmp(name, "vkGetDeviceImageMemoryRequirements"))
        return (PFN_vkVoidFunction)emu_device_image_reqs;
    return NULL;
}

static PFN_vkVoidFunction resolve(VkInstance instance, VkDevice device, const char *name)
{
    PFN_vkVoidFunction f = device ? real_gdpa(device, name) : real_gipa(instance, name);
    if (!f && (knobs & K_CORE_ALIAS)) {
        const char *alias = core_alias(name);
        if (alias) {
            f = device ? real_gdpa(device, alias) : real_gipa(instance, alias);
            if (f) fprintf(stderr, "LADDER_ALIAS %s -> %s\n", name, alias);
        }
    }
    if (!f && (f = emulated(device, name)))
        fprintf(stderr, "LADDER_EMULATE %s\n", name);
    if (!f)
        fprintf(stderr, "LADDER_PROC_NULL scope=%s name=%s\n",
                device ? "device" : instance ? "instance" : "global", name);
    return traced(name, f);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                  const char *name);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                    const char *name)
{
    init();
    if (!name) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
        return (PFN_vkVoidFunction)shim_enumerate_instance_extensions;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)shim_create_instance;
    if (instance && !strcmp(name, "vkCreateDevice")) {
        real_create_device = (PFN_vkCreateDevice)real_gipa(instance, name);
        real_get_features = (PFN_vkGetPhysicalDeviceFeatures)real_gipa(instance,
            "vkGetPhysicalDeviceFeatures");
        return real_create_device ? (PFN_vkVoidFunction)shim_create_device : NULL;
    }
    if ((knobs & K_SURFACE) && !real_gipa(instance, name)) {
        if (!strcmp(name, "vkDestroySurfaceKHR")) return (PFN_vkVoidFunction)stub_destroy_surface;
        if (!strncmp(name, "vkGetPhysicalDeviceSurface", 26) ||
            (!strncmp(name, "vkCreate", 8) && strstr(name, "SurfaceKHR")) ||
            (!strncmp(name, "vkGetPhysicalDevice", 19) && strstr(name, "PresentationSupportKHR")))
            return (PFN_vkVoidFunction)stub_surface_unsupported;
    }
    return resolve(instance, NULL, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                  const char *name)
{
    init();
    if (!name) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    return resolve(NULL, device, name);
}

/* SDL and other consumers may dlsym these directly. */
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer,
    uint32_t *count, VkExtensionProperties *out)
{ init(); return shim_enumerate_instance_extensions(layer, count, out); }
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *out)
{ init(); return shim_create_instance(info, allocator, out); }
