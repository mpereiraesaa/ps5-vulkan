#include "vk_internal.h"
#include "compilation_cache.h"
#include "physical_device_profile.h"
#include "wsi_present_backend.h"
#include <string.h>

#define INVALID VK_ERROR_UNKNOWN

static const VkExtensionProperties instance_extensions[] = {
    {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
     VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION},
    {VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME,
     VK_KHR_DEVICE_GROUP_CREATION_SPEC_VERSION},
    {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
    {VK_KHR_DISPLAY_EXTENSION_NAME, VK_KHR_DISPLAY_SPEC_VERSION},
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

/* The Vulkan 1.0 core features this driver can report, each behind exactly one
 * platform bit: a member is reported true only when the platform mask carries
 * its bit, and a request for it is honoured only under the same condition.
 * Every other VkPhysicalDeviceFeatures member is reported false and refused
 * before a device opens, whatever the platform could do. */
static const struct core_feature_bit {
    size_t offset;
    uint32_t bit;
} core_feature_bits[] = {
    {offsetof(VkPhysicalDeviceFeatures, robustBufferAccess),
     PS5VK_FEATURE_ROBUST_BUFFER_ACCESS},
    {offsetof(VkPhysicalDeviceFeatures, fullDrawIndexUint32),
     PS5VK_FEATURE_FULL_DRAW_INDEX_UINT32},
    {offsetof(VkPhysicalDeviceFeatures, multiDrawIndirect),
     PS5VK_FEATURE_MULTI_DRAW_INDIRECT},
    {offsetof(VkPhysicalDeviceFeatures, drawIndirectFirstInstance),
     PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE},
    /* User-defined distances: the pre-raster stage exports them through the
     * packed position registers and the fragment stage reads them back. Both
     * halves run on the graphics path and both are native-witnessed (the
     * eleven-case clip/cull witness verifies the export, the dynamically
     * indexed write and the pixel read), so the two members carry the platform
     * bits the graphics build sets - see src/device_profile_report.h for the
     * three distance limits, which the profile reports at the Vulkan floor of
     * eight because that is the width the two registers hold. */
    {offsetof(VkPhysicalDeviceFeatures, shaderClipDistance),
     PS5VK_FEATURE_SHADER_CLIP_DISTANCE},
    {offsetof(VkPhysicalDeviceFeatures, shaderCullDistance),
     PS5VK_FEATURE_SHADER_CULL_DISTANCE},
    /* The optional geometry stage. The graphics path compiles the merged
     * vertex+geometry pre-raster program, packages it, and runs it: the ES->GS
     * input handoff, the point and line input families, the per-primitive id,
     * gl_InvocationID and the five mandatory minima are all hardware-witnessed
     * on exactly this build (private-captures/t04), so the platform bit the
     * graphics build sets is what makes the member report true. Its five limits
     * are reported at the Vulkan floor in src/graphics_limits.h. */
    {offsetof(VkPhysicalDeviceFeatures, geometryShader),
     PS5VK_FEATURE_GEOMETRY_SHADER},
    /* Tessellation negotiation and reporting follow the validated T04 native
     * platform bit. */
    {offsetof(VkPhysicalDeviceFeatures, tessellationShader),
     PS5VK_FEATURE_TESSELLATION_SHADER},
    /* Rasterization/viewport state (DXVK262-T05). The pipeline and command
     * frontends gate the corresponding create-info and setter values on the
     * ENABLED mask, and native/draw_state_ps5.c programs the state; a platform
     * sets a bit only once that path was measured on it. */
    {offsetof(VkPhysicalDeviceFeatures, depthBiasClamp),
     PS5VK_FEATURE_DEPTH_BIAS_CLAMP},
    {offsetof(VkPhysicalDeviceFeatures, depthClamp),
     PS5VK_FEATURE_DEPTH_CLAMP},
    {offsetof(VkPhysicalDeviceFeatures, fillModeNonSolid),
     PS5VK_FEATURE_FILL_MODE_NON_SOLID},
    {offsetof(VkPhysicalDeviceFeatures, multiViewport),
     PS5VK_FEATURE_MULTI_VIEWPORT},
    /* DXVK262-T06 fragment-output capabilities. These entries only connect
     * public reporting and logical-device enablement to platform-owned bits;
     * they do not advertise anything by themselves. native/platform_ps5.c
     * therefore remains false until each native contract has its own hardware
     * witness and focused CTS result. */
    {offsetof(VkPhysicalDeviceFeatures, independentBlend),
     PS5VK_FEATURE_INDEPENDENT_BLEND},
    {offsetof(VkPhysicalDeviceFeatures, dualSrcBlend),
     PS5VK_FEATURE_DUAL_SRC_BLEND},
    {offsetof(VkPhysicalDeviceFeatures, fragmentStoresAndAtomics),
     PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS},
    {offsetof(VkPhysicalDeviceFeatures, sampleRateShading),
     PS5VK_FEATURE_SAMPLE_RATE_SHADING},
    /* A Vulkan 1.0 prerequisite for extended subgroup integer types. This
     * route is dormant until the platform carries a measured capability. */
    {offsetof(VkPhysicalDeviceFeatures, shaderInt16),
     PS5VK_FEATURE_SHADER_INT16},
    /* T07 sampled-image capabilities. These mappings only expose the public
     * query/enable path when the platform supplies its bit. */
    {offsetof(VkPhysicalDeviceFeatures, imageCubeArray),
     PS5VK_FEATURE_IMAGE_CUBE_ARRAY},
    {offsetof(VkPhysicalDeviceFeatures, textureCompressionBC),
     PS5VK_FEATURE_TEXTURE_COMPRESSION_BC},
    /* Precise occlusion queries are a Vulkan 1.0 core feature. The query
     * implementation stays dormant unless the platform supplies its bit;
     * mapping it here keeps reporting and logical-device enablement aligned. */
    {offsetof(VkPhysicalDeviceFeatures, occlusionQueryPrecise),
     PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE},
    {offsetof(VkPhysicalDeviceFeatures, shaderImageGatherExtended),
     PS5VK_FEATURE_SHADER_IMAGE_GATHER_EXTENDED},
};

static void get_core_features(const struct ps5vk_platform *platform,
                              VkPhysicalDeviceFeatures *features)
{
    memset(features, 0, sizeof(*features));
    for (size_t n = 0; n < sizeof(core_feature_bits) / sizeof(core_feature_bits[0]); ++n) {
        if (!(platform->supported_features & core_feature_bits[n].bit)) continue;
        const VkBool32 reported = VK_TRUE;
        memcpy((unsigned char *)features + core_feature_bits[n].offset,
               &reported, sizeof(reported));
    }
}

static VkResult enable_core_features(const VkPhysicalDeviceFeatures *requested,
                                     uint32_t supported, uint32_t *enabled)
{
    if (!requested) return VK_SUCCESS;
    const unsigned char *bytes = (const unsigned char *)requested;
    for (size_t offset = 0; offset < sizeof(*requested); offset += sizeof(VkBool32)) {
        VkBool32 value;
        memcpy(&value, bytes + offset, sizeof(value));
        if (!value) continue;
        if (value != VK_TRUE) return INVALID;
        const struct core_feature_bit *entry = NULL;
        for (size_t n = 0; n < sizeof(core_feature_bits) / sizeof(core_feature_bits[0]); ++n)
            if (core_feature_bits[n].offset == offset) entry = &core_feature_bits[n];
        /* A member without a platform bit, or whose bit the platform does not
         * carry, is refused before a backend/device is opened. */
        if (!entry || !(supported & entry->bit)) return VK_ERROR_FEATURE_NOT_PRESENT;
        *enabled |= entry->bit;
    }
    return VK_SUCCESS;
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
    VkBool32 features2_enabled = VK_FALSE, device_group_creation_enabled = VK_FALSE;
    VkBool32 surface_enabled = VK_FALSE, display_enabled = VK_FALSE;
    if (info->enabledExtensionCount && !info->ppEnabledExtensionNames) return INVALID;
    for (uint32_t n = 0; n < info->enabledExtensionCount; ++n) {
        const char *name = info->ppEnabledExtensionNames[n];
        if (!name) return INVALID;
        if (!strcmp(name, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            if (features2_enabled) return INVALID;
            features2_enabled = VK_TRUE;
        } else if (!strcmp(name, VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME)) {
            if (device_group_creation_enabled) return INVALID;
            device_group_creation_enabled = VK_TRUE;
        } else if (!strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME)) {
            if (surface_enabled) return INVALID;
            surface_enabled = VK_TRUE;
        } else if (!strcmp(name, VK_KHR_DISPLAY_EXTENSION_NAME)) {
            if (display_enabled) return INVALID;
            display_enabled = VK_TRUE;
        } else return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (display_enabled && !surface_enabled) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info->pNext || info->flags) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t api_version = VK_API_VERSION_1_0;
    if (info->pApplicationInfo) {
        const VkApplicationInfo *a = info->pApplicationInfo;
        if (a->sType != VK_STRUCTURE_TYPE_APPLICATION_INFO || a->pNext) return INVALID;
        /* A Vulkan 1.1 instance must not return VK_ERROR_INCOMPATIBLE_DRIVER
         * for any apiVersion unless an incompatible variant is requested.
         * Instance-level behaviour follows the lower of the request and the
         * instance version. */
        if (VK_API_VERSION_VARIANT(a->apiVersion)) return VK_ERROR_INCOMPATIBLE_DRIVER;
        if (a->apiVersion >= PS5VK_INSTANCE_API_VERSION)
            api_version = PS5VK_INSTANCE_API_VERSION;
    }
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkInstance i = ps5vk_object_alloc(NULL, allocator, sizeof(*i),
        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE, &saved, &custom);
    if (!i) return VK_ERROR_OUT_OF_HOST_MEMORY;
    i->allocator = saved; i->custom_allocator = custom;
    i->features2_extension_enabled = features2_enabled;
    i->device_group_creation_enabled = device_group_creation_enabled;
    i->surface_extension_enabled = surface_enabled;
    i->display_extension_enabled = display_enabled;
    i->api_version = api_version;
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
    if (i->devices || i->surfaces) { ++i->lifetime_errors; return; }
    VkAllocationCallbacks a = i->allocator; VkBool32 custom = i->custom_allocator;
    ps5vk_object_free(i, &a, custom);
}

/* One fixed VideoOut display and mode. These handles identify the route; a
 * mode creation request can select only the existing fixed timing. */
enum { WSI_WIDTH = 1920, WSI_HEIGHT = 1080, WSI_REFRESH_MILLIHZ = 60000 };
static VkDisplayKHR wsi_display(void) { return (VkDisplayKHR)(uintptr_t)1; }
static VkDisplayModeKHR wsi_mode(void) { return (VkDisplayModeKHR)(uintptr_t)2; }
static int display_ready(VkPhysicalDevice p)
{
    return p && p->instance && p->instance->display_extension_enabled &&
           (p->platform.queue_flags & VK_QUEUE_GRAPHICS_BIT);
}
static int surface_valid(VkPhysicalDevice p, VkSurfaceKHR surface)
{
    if (!p || !p->instance || !p->instance->surface_extension_enabled || !surface)
        return 0;
    for (VkSurfaceKHR current = p->instance->surfaces; current; current = current->next)
        if (current == surface) return 1;
    return 0;
}
/* VK_KHR_swapchain is a device extension. The fixed surface route can be
 * queried without it, but a physical device may offer presentation only when
 * its native bridge and the exact two-image BGRA8 usage are both available. */
static int swapchain_supported(VkPhysicalDevice p)
{
    if (!p || !p->instance || !p->instance->surface_extension_enabled ||
        !(p->platform.queue_flags & VK_QUEUE_GRAPHICS_BIT) ||
        !p->platform.configure || !p->platform.format_properties ||
        !p->platform.image_properties ||
        p->platform.max_allocation < UINT64_C(128) * 1024 * 1024 ||
        !ps5vk_wsi_present_available())
        return 0;
    VkFormatProperties format = {0};
    vkGetPhysicalDeviceFormatProperties(p, VK_FORMAT_B8G8R8A8_UNORM, &format);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures & required) != required) return 0;
    VkImageFormatProperties image = {0};
    if (vkGetPhysicalDeviceImageFormatProperties(p, VK_FORMAT_B8G8R8A8_UNORM,
            VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            0, &image) != VK_SUCCESS)
        return 0;
    return image.maxExtent.width >= WSI_WIDTH &&
           image.maxExtent.height >= WSI_HEIGHT && image.maxExtent.depth >= 1 &&
           image.maxMipLevels >= 1 && image.maxArrayLayers >= 1 &&
           (image.sampleCounts & VK_SAMPLE_COUNT_1_BIT);
}
static VkResult enumerate_one(const void *value, size_t size, uint32_t *count, void *out)
{
    if (!count) return INVALID;
    if (!out) { *count = 1; return VK_SUCCESS; }
    if (!*count) return VK_INCOMPLETE;
    memcpy(out, value, size);
    *count = 1;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceDisplayPropertiesKHR(
    VkPhysicalDevice p, uint32_t *count, VkDisplayPropertiesKHR *out)
{
    if (!display_ready(p)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkDisplayPropertiesKHR value = {
        .display = wsi_display(), .displayName = "PS5 VideoOut",
        .physicalResolution = {WSI_WIDTH, WSI_HEIGHT},
        .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR};
    return enumerate_one(&value, sizeof(value), count, out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayModePropertiesKHR(
    VkPhysicalDevice p, VkDisplayKHR display, uint32_t *count,
    VkDisplayModePropertiesKHR *out)
{
    if (!display_ready(p)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (display != wsi_display()) return VK_ERROR_INITIALIZATION_FAILED;
    const VkDisplayModePropertiesKHR value = {
        .displayMode = wsi_mode(),
        .parameters = {{WSI_WIDTH, WSI_HEIGHT}, WSI_REFRESH_MILLIHZ}};
    return enumerate_one(&value, sizeof(value), count, out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDisplayModeKHR(
    VkPhysicalDevice p, VkDisplayKHR display, const VkDisplayModeCreateInfoKHR *info,
    const VkAllocationCallbacks *allocator, VkDisplayModeKHR *out)
{
    (void)allocator;
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!display_ready(p)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (display != wsi_display() || !info ||
        info->sType != VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR ||
        info->pNext || info->flags ||
        info->parameters.visibleRegion.width != WSI_WIDTH ||
        info->parameters.visibleRegion.height != WSI_HEIGHT ||
        info->parameters.refreshRate != WSI_REFRESH_MILLIHZ)
        return VK_ERROR_INITIALIZATION_FAILED;
    *out = wsi_mode();
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceDisplayPlanePropertiesKHR(
    VkPhysicalDevice p, uint32_t *count, VkDisplayPlanePropertiesKHR *out)
{
    if (!display_ready(p)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkDisplayPlanePropertiesKHR value = {wsi_display(), 0};
    return enumerate_one(&value, sizeof(value), count, out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayPlaneSupportedDisplaysKHR(
    VkPhysicalDevice p, uint32_t plane, uint32_t *count, VkDisplayKHR *out)
{
    if (!display_ready(p)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (plane) return VK_ERROR_INITIALIZATION_FAILED;
    const VkDisplayKHR value = wsi_display();
    return enumerate_one(&value, sizeof(value), count, out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayPlaneCapabilitiesKHR(
    VkPhysicalDevice p, VkDisplayModeKHR mode, uint32_t plane,
    VkDisplayPlaneCapabilitiesKHR *out)
{
    if (!display_ready(p)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!out) return INVALID;
    if (mode != wsi_mode() || plane) return VK_ERROR_INITIALIZATION_FAILED;
    *out = (VkDisplayPlaneCapabilitiesKHR){
        .supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .minSrcExtent = {WSI_WIDTH, WSI_HEIGHT},
        .maxSrcExtent = {WSI_WIDTH, WSI_HEIGHT},
        .minDstExtent = {WSI_WIDTH, WSI_HEIGHT},
        .maxDstExtent = {WSI_WIDTH, WSI_HEIGHT}};
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDisplayPlaneSurfaceKHR(
    VkInstance i, const VkDisplaySurfaceCreateInfoKHR *info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!i || !i->surface_extension_enabled || !i->display_extension_enabled)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!(i->physical.platform.queue_flags & VK_QUEUE_GRAPHICS_BIT))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!info || info->sType != VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR ||
        info->pNext || info->flags || info->displayMode != wsi_mode() ||
        info->planeIndex || info->planeStackIndex ||
        info->transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
        info->alphaMode != VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR ||
        info->imageExtent.width != WSI_WIDTH || info->imageExtent.height != WSI_HEIGHT)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkSurfaceKHR surface = ps5vk_object_alloc(i->custom_allocator ? &i->allocator : NULL,
        allocator, sizeof(*surface), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!surface) return VK_ERROR_OUT_OF_HOST_MEMORY;
    surface->instance = i;
    surface->extent = info->imageExtent;
    surface->allocator = saved;
    surface->custom_allocator = custom;
    surface->next = i->surfaces;
    i->surfaces = surface;
    *out = surface;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance i, VkSurfaceKHR surface,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!i || !surface) return;
    VkSurfaceKHR *link = &i->surfaces;
    while (*link && *link != surface) link = &(*link)->next;
    if (!*link) { ++i->lifetime_errors; return; }
    if (surface->swapchains) { ++i->lifetime_errors; return; }
    *link = surface->next;
    VkAllocationCallbacks saved = surface->allocator;
    VkBool32 custom = surface->custom_allocator;
    ps5vk_object_free(surface, &saved, custom);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice p, uint32_t family, VkSurfaceKHR surface, VkBool32 *out)
{
    if (!out) return INVALID;
    *out = VK_FALSE;
    if (!surface_valid(p, surface)) return VK_ERROR_SURFACE_LOST_KHR;
    if (family) return VK_ERROR_INITIALIZATION_FAILED;
    *out = swapchain_supported(p) ? VK_TRUE : VK_FALSE;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice p, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *out)
{
    if (!out) return INVALID;
    if (!surface_valid(p, surface)) return VK_ERROR_SURFACE_LOST_KHR;
    *out = (VkSurfaceCapabilitiesKHR){
        .minImageCount = 2, .maxImageCount = 2,
        .currentExtent = {WSI_WIDTH, WSI_HEIGHT},
        .minImageExtent = {WSI_WIDTH, WSI_HEIGHT},
        .maxImageExtent = {WSI_WIDTH, WSI_HEIGHT},
        .maxImageArrayLayers = 1,
        .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .supportedUsageFlags = swapchain_supported(p) ?
            (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) :
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice p, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *out)
{
    if (!surface_valid(p, surface)) return VK_ERROR_SURFACE_LOST_KHR;
    const VkSurfaceFormatKHR value = {VK_FORMAT_B8G8R8A8_UNORM,
                                      VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    return enumerate_one(&value, sizeof(value), count, out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice p, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *out)
{
    if (!surface_valid(p, surface)) return VK_ERROR_SURFACE_LOST_KHR;
    const VkPresentModeKHR value = VK_PRESENT_MODE_FIFO_KHR;
    return enumerate_one(&value, sizeof(value), count, out);
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
static VkResult enumerate_physical_device_groups(VkInstance i,
    uint32_t *count, VkPhysicalDeviceGroupProperties *out)
{
    if (!count) return INVALID;
    if (!out) { *count = 1; return VK_SUCCESS; }
    if (!*count) return VK_INCOMPLETE;
    if (out[0].sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES)
        return INVALID;
    out[0].physicalDeviceCount = 1;
    out[0].physicalDevices[0] = &i->physical;
    for (uint32_t n = 1; n < VK_MAX_DEVICE_GROUP_SIZE; ++n)
        out[0].physicalDevices[n] = VK_NULL_HANDLE;
    out[0].subsetAllocation = VK_FALSE;
    *count = 1;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroupsKHR(VkInstance i,
    uint32_t *count, VkPhysicalDeviceGroupProperties *out)
{
    if (!i || !i->device_group_creation_enabled) return INVALID;
    return enumerate_physical_device_groups(i, count, out);
}
/* Core Vulkan 1.1 name: an instance-level command of a 1.1 instance. */
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(VkInstance i,
    uint32_t *count, VkPhysicalDeviceGroupProperties *out)
{
    if (!i || i->api_version < VK_API_VERSION_1_1) return INVALID;
    return enumerate_physical_device_groups(i, count, out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *version)
{
    if (!version) return INVALID;
    *version = PS5VK_INSTANCE_API_VERSION;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice p,
                                                         VkPhysicalDeviceProperties *out)
{ if (p && out) *out = p->platform.properties; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p,
                                                              VkPhysicalDeviceMemoryProperties *out)
{ if (p && out) *out = p->platform.memory_properties; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice p, VkPhysicalDeviceFeatures *out)
{ if (p && out) get_core_features(&p->platform, out); }
/* VK_KHR_maintenance2 has no registry dependency on Vulkan 1.0. */
static VkBool32 maintenance2_supported(VkPhysicalDevice p)
{
    return (p->platform.supported_features_t09 & PS5VK_T09_FEATURE_MAINTENANCE2) != 0;
}
/* VK_KHR_create_renderpass2 depends, in the pinned registry, on
 * VK_KHR_multiview and VK_KHR_maintenance2 (or Vulkan 1.1, which this 1.0
 * profile is not), so it is reported only when both are. */
static VkBool32 create_renderpass2_supported(VkPhysicalDevice p)
{
    return (p->platform.supported_features_t09 & PS5VK_T09_FEATURE_CREATE_RENDERPASS2) &&
        maintenance2_supported(p) &&
        (p->platform.supported_features & PS5VK_FEATURE_MULTIVIEW);
}
/* VK_KHR_separate_depth_stencil_layouts depends, in the pinned registry, on
 * VK_KHR_get_physical_device_properties2 and VK_KHR_create_renderpass2. The
 * instance extension is checked at device creation; the device route is
 * create_renderpass2. */
static VkBool32 separate_depth_stencil_route(VkPhysicalDevice p)
{
    return create_renderpass2_supported(p);
}
/* VK_KHR_get_memory_requirements2 and VK_KHR_bind_memory2 have no registry
 * dependency on Vulkan 1.0; VK_KHR_dedicated_allocation depends on
 * VK_KHR_get_memory_requirements2, so it is reported only with it. */
static VkBool32 memory_requirements2_supported(VkPhysicalDevice p)
{
    return (p->platform.supported_features_t09 &
            PS5VK_T09_FEATURE_GET_MEMORY_REQUIREMENTS2) != 0;
}
static VkBool32 dedicated_allocation_supported(VkPhysicalDevice p)
{
    return (p->platform.supported_features_t09 & PS5VK_T09_FEATURE_DEDICATED_ALLOCATION) &&
        memory_requirements2_supported(p);
}
static VkBool32 bind_memory2_supported(VkPhysicalDevice p)
{
    return (p->platform.supported_features_t09 & PS5VK_T09_FEATURE_BIND_MEMORY2) != 0;
}
/* VK_KHR_maintenance4 depends on VK_VERSION_1_1 in the pinned registry (no
 * extension alternative): a Vulkan 1.0 device cannot enable it, so the route
 * opens only when both the platform bit and the device version allow it. */
static VkBool32 maintenance4_supported(VkPhysicalDevice p)
{
    return (p->platform.supported_features_t09 & PS5VK_T09_FEATURE_MAINTENANCE4) &&
        (p->platform.properties.apiVersion >= VK_API_VERSION_1_1 ||
         p->platform.maintenance4_diagnostic_on_vulkan_1_0);
}
/* The DXVK first-draw recording routes (DXVK262-T10), each an unadvertised
 * platform capability until its native witness promotes it. Each pinned
 * registry entry depends on VK_KHR_get_physical_device_properties2 (or Vulkan
 * 1.1, which this 1.0 profile is not); vkCreateDevice enforces that. */
static VkBool32 extended_dynamic_state_supported(VkPhysicalDevice p)
{
    return (p->platform.supported_features_t09 & PS5VK_T09_FEATURE_EXTENDED_DYNAMIC_STATE) != 0;
}
static VkBool32 separate_depth_stencil_supported(VkPhysicalDevice p)
{
    return (p->platform.supported_features_t09 &
            PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS) &&
        separate_depth_stencil_route(p);
}
/* VK_EXT_robustness2 depends, in the pinned registry, on
 * VK_KHR_get_physical_device_properties2 or Vulkan 1.1; this profile's device
 * is 1.0, so vkCreateDevice requires the instance extension. The extension is
 * reported once the platform carries either of its implemented features. */
static VkBool32 robustness2_supported(VkPhysicalDevice p)
{
    return (p->platform.supported_features_t09 &
            (PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2 |
             PS5VK_T09_FEATURE_NULL_DESCRIPTOR)) != 0;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice p,
                                                           VkPhysicalDeviceFeatures2 *out)
{
    if (!p || !out || out->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
        return;
    get_core_features(&p->platform, &out->features);
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
            ((VkPhysicalDeviceShaderDrawParametersFeatures *)next)->shaderDrawParameters =
                !!(p->platform.supported_features & PS5VK_FEATURE_SHADER_DRAW_PARAMETERS);
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) {
            /* The queried feature follows the internal capability exactly, and
             * only the multiview core feature: geometry and tessellation
             * shader multiview are not implemented and are reported false. */
            VkPhysicalDeviceMultiviewFeatures *features =
                (VkPhysicalDeviceMultiviewFeatures *)next;
            features->multiview =
                (VkBool32)ps5vk_platform_multiview_supported(p->platform.supported_features);
            features->multiviewGeometryShader = VK_FALSE;
            features->multiviewTessellationShader = VK_FALSE;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR) {
            VkPhysicalDeviceVulkanMemoryModelFeaturesKHR *features =
                (VkPhysicalDeviceVulkanMemoryModelFeaturesKHR *)next;
            const uint32_t supported = p->platform.supported_features;
            features->vulkanMemoryModel =
                !!(supported & PS5VK_FEATURE_VULKAN_MEMORY_MODEL);
            features->vulkanMemoryModelDeviceScope =
                !!((supported & PS5VK_FEATURE_VULKAN_MEMORY_MODEL) &&
                   (supported & PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE));
            features->vulkanMemoryModelAvailabilityVisibilityChains = VK_FALSE;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR) {
            VkPhysicalDeviceBufferDeviceAddressFeaturesKHR *features =
                (VkPhysicalDeviceBufferDeviceAddressFeaturesKHR *)next;
            features->bufferDeviceAddress =
                !!(p->platform.supported_features & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS);
            features->bufferDeviceAddressCaptureReplay = VK_FALSE;
            features->bufferDeviceAddressMultiDevice = VK_FALSE;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES) {
            ((VkPhysicalDeviceUniformBufferStandardLayoutFeatures *)next)->uniformBufferStandardLayout =
                !!(p->platform.supported_features & PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT);
        } else if (next->sType ==

                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES) {
            ((VkPhysicalDeviceHostQueryResetFeatures *)next)->hostQueryReset =
                !!(p->platform.supported_features_t09 & PS5VK_T09_FEATURE_HOST_QUERY_RESET);
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES) {
            ((VkPhysicalDeviceImagelessFramebufferFeatures *)next)->imagelessFramebuffer =
                !!(p->platform.supported_features_t09 & PS5VK_T09_FEATURE_IMAGELESS_FRAMEBUFFER);
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            ((VkPhysicalDeviceTimelineSemaphoreFeatures *)next)->timelineSemaphore =
                !!(p->platform.supported_features_t09 & PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE);
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES) {
            ((VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures *)next)
                ->separateDepthStencilLayouts = separate_depth_stencil_supported(p);
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES) {
            ((VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures *)next)
                ->shaderDemoteToHelperInvocation = !!(p->platform.supported_features_t09 &
                    PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION);
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES) {
            ((VkPhysicalDeviceShaderTerminateInvocationFeatures *)next)
                ->shaderTerminateInvocation = !!(p->platform.supported_features_t09 &
                    PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION);
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES) {
            ((VkPhysicalDeviceMaintenance4Features *)next)->maintenance4 =
                maintenance4_supported(p);
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            VkPhysicalDeviceRobustness2FeaturesEXT *features =
                (VkPhysicalDeviceRobustness2FeaturesEXT *)next;
            features->robustBufferAccess2 = !!(p->platform.supported_features_t09 &
                PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2);
            features->robustImageAccess2 = VK_FALSE;
            features->nullDescriptor = !!(p->platform.supported_features_t09 &
                PS5VK_T09_FEATURE_NULL_DESCRIPTOR);

        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT) {
            ((VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *)next)->extendedDynamicState =
                extended_dynamic_state_supported(p);
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice p,
                                                              VkPhysicalDeviceProperties2 *out)
{
    if (!p || !out || out->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2)
        return;
    vkGetPhysicalDeviceProperties(p, &out->properties);
    for (VkBaseOutStructure *next = (VkBaseOutStructure *)out->pNext; next;
         next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES) {
            /* The Vulkan 1.0 profile has no reported subgroup stages or
             * operations. Answer all fields rather than retaining the
             * caller's previous values as apparent capabilities. */
            VkPhysicalDeviceSubgroupProperties *properties =
                (VkPhysicalDeviceSubgroupProperties *)next;
            properties->subgroupSize = 0u;
            properties->supportedStages = 0u;
            properties->supportedOperations = 0u;
            properties->quadOperationsInAllStages = VK_FALSE;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES) {
            /* ONLY the floors this profile measured, and zero when the platform
             * does not carry the capability: no invented maxima. */
            VkPhysicalDeviceMultiviewProperties *properties =
                (VkPhysicalDeviceMultiviewProperties *)next;
            const int supported =
                ps5vk_platform_multiview_supported(p->platform.supported_features);
            properties->maxMultiviewViewCount =
                supported ? (uint32_t)PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR : 0u;
            properties->maxMultiviewInstanceIndex =
                supported ? (uint32_t)PS5VK_MULTIVIEW_INSTANCE_INDEX_FLOOR : 0u;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES) {
            /* The payload algorithm's own bound (src/vk_internal.h), and
             * zero when the platform does not carry the capability. */
            ((VkPhysicalDeviceTimelineSemaphoreProperties *)next)
                ->maxTimelineSemaphoreValueDifference =
                (p->platform.supported_features_t09 & PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE) ?
                PS5VK_TIMELINE_MAX_VALUE_DIFFERENCE : 0u;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES) {
            /* VK_KHR_maintenance2. ALL_CLIP_PLANES is the Vulkan 1.0 rule this
             * device already obeys - a point outside the clip volume is
             * discarded - so reporting it claims no new behaviour; the
             * USER_CLIP_PLANES_ONLY relaxation is never claimed. */
            ((VkPhysicalDevicePointClippingProperties *)next)->pointClippingBehavior =
                VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES) {
            /* The largest size vkCreateBuffer accepts: its aligned footprint
             * must fit one allocation. This is the allocator's software
             * budget, below the 2^30 minimum the extension sets. */
            const VkDeviceSize alignment =
                p->platform.properties.limits.minStorageBufferOffsetAlignment;
            ((VkPhysicalDeviceMaintenance4Properties *)next)->maxBufferSize =
                alignment ? p->platform.max_allocation & ~(alignment - 1) : 0;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT) {
            /* The granularity the buffer encoder rounds NUM_RECORDS to (see
             * src/descriptor_encode.c), and zero without robustBufferAccess2. */
            VkPhysicalDeviceRobustness2PropertiesEXT *properties =
                (VkPhysicalDeviceRobustness2PropertiesEXT *)next;
            const VkDeviceSize alignment = (p->platform.supported_features_t09 &
                PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2) ?
                PS5VK_ROBUST_BUFFER_ACCESS_SIZE_ALIGNMENT : 0u;
            properties->robustStorageBufferAccessSizeAlignment = alignment;
            properties->robustUniformBufferAccessSizeAlignment = alignment;
        }
    }
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
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice p, VkFormat format, VkImageType type,
    VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling,
    uint32_t *count, VkSparseImageFormatProperties *out)
{
    (void)format; (void)type; (void)samples; (void)usage; (void)tiling; (void)out;
    if (!p || !count) return;
    /* The 1.0 form of the same report: sparse binding is not advertised, images
     * cannot be created with sparse flags, and the valid answer is empty. */
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

    /* Twenty-five conditional pushes follow (storage class, 8-bit, 16-bit, draw
     * parameters, multiview, memory model, device group, buffer address, UBO
     * layout, host query reset, sampler mirror clamp, timeline, maintenance2,
     * create_renderpass2, separate depth/stencil layouts, swapchain, demote to
     * helper invocation, terminate invocation, get_memory_requirements2,
     * dedicated_allocation, bind_memory2, maintenance4, descriptor update
     * template, robustness2, extended dynamic state). Keep headroom so a new
     * entry cannot overflow the array before this bound is revisited; each push
     * site must stay below it. */
    enum { DEVICE_EXTENSION_PUSHES = 25, DEVICE_EXTENSION_SLOTS = 28 };
    _Static_assert(DEVICE_EXTENSION_PUSHES <= DEVICE_EXTENSION_SLOTS,
                   "device extension array too small");
    VkExtensionProperties properties[DEVICE_EXTENSION_SLOTS];

    uint32_t total = 0;
    if (swapchain_supported(p)) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION};
    }
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
    if (p->platform.supported_features & PS5VK_FEATURE_SHADER_DRAW_PARAMETERS) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME,
            VK_KHR_SHADER_DRAW_PARAMETERS_SPEC_VERSION};
    }
    if (p->platform.supported_features & PS5VK_FEATURE_MULTIVIEW) {
        properties[total++] = (VkExtensionProperties){VK_KHR_MULTIVIEW_EXTENSION_NAME,
                                                      VK_KHR_MULTIVIEW_SPEC_VERSION};
    }
    if (p->platform.supported_features & PS5VK_FEATURE_VULKAN_MEMORY_MODEL) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME,
            VK_KHR_VULKAN_MEMORY_MODEL_SPEC_VERSION};
    }
    /* The group route is needed for KHR buffer addresses on Vulkan 1.0. Both
     * names stay absent from the shipping build until the platform enables
     * its measured buffer-address capability. */
    if (p->platform.supported_features & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_DEVICE_GROUP_EXTENSION_NAME, VK_KHR_DEVICE_GROUP_SPEC_VERSION};
        properties[total++] = (VkExtensionProperties){
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
            VK_KHR_BUFFER_DEVICE_ADDRESS_SPEC_VERSION};
    }
    if (p->platform.supported_features & PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME,
            VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_SPEC_VERSION};
    }

    if (p->platform.supported_features_t09 & PS5VK_T09_FEATURE_HOST_QUERY_RESET) {
        properties[total++] = (VkExtensionProperties){
            VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME, VK_EXT_HOST_QUERY_RESET_SPEC_VERSION};
    }
    if (p->platform.supported_features_t09 &
        PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME,
            VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_SPEC_VERSION};
    }

    if (p->platform.supported_features_t09 & PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
            VK_KHR_TIMELINE_SEMAPHORE_SPEC_VERSION};
    }
    if (maintenance2_supported(p)) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_MAINTENANCE_2_EXTENSION_NAME, VK_KHR_MAINTENANCE_2_SPEC_VERSION};
    }
    if (create_renderpass2_supported(p)) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_SPEC_VERSION};
    }
    if (separate_depth_stencil_supported(p)) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME,
            VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_SPEC_VERSION};
    }
    if (p->platform.supported_features_t09 &
        PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION) {
        properties[total++] = (VkExtensionProperties){
            VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME,
            VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_SPEC_VERSION};
    }
    if (p->platform.supported_features_t09 & PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME,
            VK_KHR_SHADER_TERMINATE_INVOCATION_SPEC_VERSION};
    }
    if (memory_requirements2_supported(p)) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_SPEC_VERSION};
    }
    if (dedicated_allocation_supported(p)) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
            VK_KHR_DEDICATED_ALLOCATION_SPEC_VERSION};
    }
    if (bind_memory2_supported(p)) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, VK_KHR_BIND_MEMORY_2_SPEC_VERSION};
    }
    if (maintenance4_supported(p)) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_MAINTENANCE_4_EXTENSION_NAME, VK_KHR_MAINTENANCE_4_SPEC_VERSION};
    }
    /* Descriptor update templates are a host-side recording of descriptor
     * writes: every update goes through vkUpdateDescriptorSets. */
    if (p->platform.supported_features_t09 & PS5VK_T09_FEATURE_DESCRIPTOR_UPDATE_TEMPLATE) {
        properties[total++] = (VkExtensionProperties){
            VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME,
            VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_SPEC_VERSION};
    }
    if (robustness2_supported(p)) {
        properties[total++] = (VkExtensionProperties){
            VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_SPEC_VERSION};
    }
    /* DXVK262-T10 recording routes. */
    if (extended_dynamic_state_supported(p)) {
        properties[total++] = (VkExtensionProperties){
            VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
            VK_EXT_EXTENDED_DYNAMIC_STATE_SPEC_VERSION};
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
    VkBool32 draw_parameters = VK_FALSE, multiview_extension = VK_FALSE;
    VkBool32 memory_model_extension = VK_FALSE;
    VkBool32 group_extension = VK_FALSE, buffer_address_extension = VK_FALSE;
    VkBool32 uniform_buffer_standard_layout_extension = VK_FALSE;

    VkBool32 host_query_reset_extension = VK_FALSE;
    VkBool32 sampler_mirror_clamp_extension = VK_FALSE;

    VkBool32 timeline_extension = VK_FALSE;
    VkBool32 separate_depth_stencil_extension = VK_FALSE;
    VkBool32 maintenance2_extension = VK_FALSE, create_renderpass2_extension = VK_FALSE;
    VkBool32 robustness2_extension = VK_FALSE;
    VkBool32 swapchain_extension = VK_FALSE;
    VkBool32 demote_extension = VK_FALSE, terminate_extension = VK_FALSE;
    VkBool32 memory_requirements2_extension = VK_FALSE;
    VkBool32 dedicated_allocation_extension = VK_FALSE, bind_memory2_extension = VK_FALSE;
    VkBool32 maintenance4_extension = VK_FALSE, saw_maintenance4 = VK_FALSE;
    VkBool32 descriptor_update_template_extension = VK_FALSE;
    /* DXVK262-T10 recording routes. */
    VkBool32 extended_dynamic_state_extension = VK_FALSE;

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
        else if (!strcmp(name, VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME))
            seen = &draw_parameters;
        else if (!strcmp(name, VK_KHR_MULTIVIEW_EXTENSION_NAME))
            seen = &multiview_extension;
        else if (!strcmp(name, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME))
            seen = &memory_model_extension;
        else if (!strcmp(name, VK_KHR_DEVICE_GROUP_EXTENSION_NAME))
            seen = &group_extension;
        else if (!strcmp(name, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
            seen = &buffer_address_extension;
        else if (!strcmp(name, VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME))
            seen = &uniform_buffer_standard_layout_extension;

        else if (!strcmp(name, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME))
            seen = &host_query_reset_extension;
        else if (!strcmp(name, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME))
            seen = &sampler_mirror_clamp_extension;

        else if (!strcmp(name, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
            seen = &timeline_extension;
        else if (!strcmp(name, VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME))
            seen = &separate_depth_stencil_extension;
        else if (!strcmp(name, VK_KHR_MAINTENANCE_2_EXTENSION_NAME))
            seen = &maintenance2_extension;
        else if (!strcmp(name, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME))
            seen = &create_renderpass2_extension;
        else if (!strcmp(name, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME))
            seen = &robustness2_extension;
        else if (!strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            seen = &swapchain_extension;
        else if (!strcmp(name, VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME))
            seen = &demote_extension;
        else if (!strcmp(name, VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME))
            seen = &terminate_extension;
        else if (!strcmp(name, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
            seen = &memory_requirements2_extension;
        else if (!strcmp(name, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME))
            seen = &dedicated_allocation_extension;
        else if (!strcmp(name, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME))
            seen = &bind_memory2_extension;
        else if (!strcmp(name, VK_KHR_MAINTENANCE_4_EXTENSION_NAME))
            seen = &maintenance4_extension;
        else if (!strcmp(name, VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME))
            seen = &descriptor_update_template_extension;
        else if (!strcmp(name, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME))
            seen = &extended_dynamic_state_extension;

        else {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        if (*seen) return INVALID;
        *seen = VK_TRUE;
    }
    if ((extension8 && !(p->platform.supported_features & PS5VK_FEATURE_STORAGE_BUFFER_8BIT)) ||
        (extension16 && !(p->platform.supported_features & PS5VK_FEATURE_STORAGE_BUFFER_16BIT)) ||
        (draw_parameters &&
         !(p->platform.supported_features & PS5VK_FEATURE_SHADER_DRAW_PARAMETERS)) ||
        ((extension8 || extension16) &&
         (!storage_class || !p->instance->features2_extension_enabled)))
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    if (swapchain_extension && !swapchain_supported(p))
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    if (multiview_extension &&
        (!(p->platform.supported_features & PS5VK_FEATURE_MULTIVIEW) ||
         !p->instance->features2_extension_enabled))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    /* On Vulkan 1.0 the KHR route depends on the instance's Features2
     * extension; an internal platform bit alone is not an enabled API route. */
    if (memory_model_extension &&
        (!(p->platform.supported_features & PS5VK_FEATURE_VULKAN_MEMORY_MODEL) ||
         !p->instance->features2_extension_enabled))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (group_extension &&
        (!(p->platform.supported_features & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS) ||
         !p->instance->device_group_creation_enabled))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (buffer_address_extension &&
        (!(p->platform.supported_features & PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS) ||
         !p->instance->features2_extension_enabled || !group_extension))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (uniform_buffer_standard_layout_extension &&
        (!(p->platform.supported_features & PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT) ||
         !p->instance->features2_extension_enabled))
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    if (host_query_reset_extension &&
        (!(p->platform.supported_features_t09 & PS5VK_T09_FEATURE_HOST_QUERY_RESET) ||
         !p->instance->features2_extension_enabled))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (sampler_mirror_clamp_extension &&
        !(p->platform.supported_features_t09 &
          PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE))
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    /* The pinned registry makes VK_KHR_timeline_semaphore depend on
     * VK_KHR_get_physical_device_properties2 or Vulkan 1.1; this profile is
     * 1.0, so only the instance extension satisfies it. */
    if (timeline_extension &&
        (!(p->platform.supported_features_t09 & PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE) ||
         !p->instance->features2_extension_enabled))
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    /* Every enabled extension's registry dependencies must be enabled too
     * (VUID-vkCreateDevice-ppEnabledExtensionNames-01387). */
    if (maintenance2_extension && !maintenance2_supported(p))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (descriptor_update_template_extension &&
        !(p->platform.supported_features_t09 & PS5VK_T09_FEATURE_DESCRIPTOR_UPDATE_TEMPLATE))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (create_renderpass2_extension &&
        (!create_renderpass2_supported(p) || !multiview_extension || !maintenance2_extension))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (separate_depth_stencil_extension &&
        (!separate_depth_stencil_supported(p) || !p->instance->features2_extension_enabled ||
         !create_renderpass2_extension))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    /* Both pixel-removal extensions depend, in the pinned registry, on
     * VK_KHR_get_physical_device_properties2 or Vulkan 1.1. */
    if (demote_extension &&
        (!(p->platform.supported_features_t09 &
           PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION) ||
         !p->instance->features2_extension_enabled))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (terminate_extension &&
        (!(p->platform.supported_features_t09 &
           PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION) ||
         !p->instance->features2_extension_enabled))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if ((memory_requirements2_extension && !memory_requirements2_supported(p)) ||
        (dedicated_allocation_extension &&
         (!dedicated_allocation_supported(p) || !memory_requirements2_extension)) ||
        (bind_memory2_extension && !bind_memory2_supported(p)) ||
        (maintenance4_extension && !maintenance4_supported(p)))
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    if (robustness2_extension &&
        (!robustness2_supported(p) || !p->instance->features2_extension_enabled))
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (extended_dynamic_state_extension &&
        (!extended_dynamic_state_supported(p) || !p->instance->features2_extension_enabled))
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    uint32_t enabled_features = 0;
    uint32_t enabled_features_t09 = sampler_mirror_clamp_extension ?
        PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE : 0;

    VkBool32 saw_features2 = VK_FALSE, saw8 = VK_FALSE, saw16 = VK_FALSE;
    VkBool32 saw_draw_parameters = VK_FALSE, saw_multiview = VK_FALSE;
    VkBool32 saw_memory_model = VK_FALSE;
    VkBool32 saw_buffer_address = VK_FALSE;
    VkBool32 saw_device_group = VK_FALSE;
    VkBool32 saw_uniform_buffer_standard_layout = VK_FALSE;
    VkBool32 saw_host_query_reset = VK_FALSE, saw_imageless_framebuffer = VK_FALSE;
    VkBool32 saw_dynamic_rendering = VK_FALSE;
    VkBool32 saw_timeline = VK_FALSE;
    VkBool32 saw_separate_depth_stencil = VK_FALSE;
    VkBool32 saw_demote = VK_FALSE, saw_terminate = VK_FALSE;
    VkBool32 saw_robustness2 = VK_FALSE;
    VkBool32 saw_extended_dynamic_state = VK_FALSE, extended_dynamic_state = VK_FALSE;
    for (const VkBaseInStructure *next = (const VkBaseInStructure *)info->pNext;
         next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO) {
            if (saw_device_group || !p->instance->device_group_creation_enabled)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            saw_device_group = VK_TRUE;
            const VkDeviceGroupDeviceCreateInfo *group =
                (const VkDeviceGroupDeviceCreateInfo *)next;
            if (group->physicalDeviceCount != 1 || !group->pPhysicalDevices ||
                group->pPhysicalDevices[0] != p)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            if (saw_features2 || info->pEnabledFeatures) return INVALID;
            saw_features2 = VK_TRUE;
            const VkPhysicalDeviceFeatures2 *features =
                (const VkPhysicalDeviceFeatures2 *)next;
            VkResult core_result = enable_core_features(&features->features,
                p->platform.supported_features, &enabled_features);
            if (core_result != VK_SUCCESS) return core_result;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            if (saw_robustness2 || !robustness2_extension)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            saw_robustness2 = VK_TRUE;
            const VkPhysicalDeviceRobustness2FeaturesEXT *features =
                (const VkPhysicalDeviceRobustness2FeaturesEXT *)next;
            if (!valid_bool(features->robustBufferAccess2) ||
                !valid_bool(features->robustImageAccess2) ||
                !valid_bool(features->nullDescriptor)) return INVALID;
            if (features->robustImageAccess2 ||
                (features->robustBufferAccess2 && !(p->platform.supported_features_t09 &
                    PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2)) ||
                (features->nullDescriptor && !(p->platform.supported_features_t09 &
                    PS5VK_T09_FEATURE_NULL_DESCRIPTOR)))
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if (features->robustBufferAccess2)
                enabled_features_t09 |= PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2;
            if (features->nullDescriptor)
                enabled_features_t09 |= PS5VK_T09_FEATURE_NULL_DESCRIPTOR;
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
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES) {
            /* The pinned CTS builds one device chain for every rendering type it
             * exercises, so it always carries VkPhysicalDeviceDynamicRendering
             * Features once Features2 is available, with the feature left at its
             * default false value. Dynamic rendering is not implemented and not
             * advertised, so accept the neutral value only: the structure
             * enables nothing, and asking for the feature fails closed with the
             * precise unsupported-feature result instead of being mistaken for
             * an unrecognised structure. */
            if (saw_dynamic_rendering) return INVALID;
            saw_dynamic_rendering = VK_TRUE;
            const VkPhysicalDeviceDynamicRenderingFeatures *features =
                (const VkPhysicalDeviceDynamicRenderingFeatures *)next;
            if (!valid_bool(features->dynamicRendering)) return INVALID;
            if (features->dynamicRendering) return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) {
            /* Enabling the extension does NOT oblige the caller to ask for the
             * feature: the structure may be absent, or present with all three
             * flags false, and the device is created with the feature disabled
             * either way. Asking for it is what has to hold up. */
            if (saw_multiview) return INVALID;
            saw_multiview = VK_TRUE;
            const VkPhysicalDeviceMultiviewFeatures *features =
                (const VkPhysicalDeviceMultiviewFeatures *)next;
            if (!valid_bool(features->multiview) ||
                !valid_bool(features->multiviewGeometryShader) ||
                !valid_bool(features->multiviewTessellationShader)) return INVALID;
            if (features->multiviewGeometryShader || features->multiviewTessellationShader)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if (features->multiview) {
                if (!multiview_extension ||
                    !(p->platform.supported_features & PS5VK_FEATURE_MULTIVIEW))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features |= PS5VK_FEATURE_MULTIVIEW;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES) {
            if (saw_uniform_buffer_standard_layout) return INVALID;
            saw_uniform_buffer_standard_layout = VK_TRUE;
            const VkPhysicalDeviceUniformBufferStandardLayoutFeatures *features =
                (const VkPhysicalDeviceUniformBufferStandardLayoutFeatures *)next;
            if (!valid_bool(features->uniformBufferStandardLayout)) return INVALID;
            if (features->uniformBufferStandardLayout) {
                if (!uniform_buffer_standard_layout_extension ||
                    !(p->platform.supported_features & PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features |= PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES) {
            if (saw_host_query_reset) return INVALID;
            saw_host_query_reset = VK_TRUE;
            const VkPhysicalDeviceHostQueryResetFeatures *features =
                (const VkPhysicalDeviceHostQueryResetFeatures *)next;
            if (!valid_bool(features->hostQueryReset)) return INVALID;
            if (features->hostQueryReset) {
                if (!host_query_reset_extension ||
                    !(p->platform.supported_features_t09 & PS5VK_T09_FEATURE_HOST_QUERY_RESET))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features_t09 |= PS5VK_T09_FEATURE_HOST_QUERY_RESET;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES) {
            if (saw_imageless_framebuffer) return INVALID;
            saw_imageless_framebuffer = VK_TRUE;
            const VkPhysicalDeviceImagelessFramebufferFeatures *features =
                (const VkPhysicalDeviceImagelessFramebufferFeatures *)next;
            if (!valid_bool(features->imagelessFramebuffer)) return INVALID;
            if (features->imagelessFramebuffer) {
                /* Diagnostic execution only. The Vulkan 1.0 KHR extension is
                 * not enumerated until maintenance2 and image_format_list are
                 * implemented as public dependencies. */
                if (!(p->platform.supported_features_t09 &
                      PS5VK_T09_FEATURE_IMAGELESS_FRAMEBUFFER))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features_t09 |= PS5VK_T09_FEATURE_IMAGELESS_FRAMEBUFFER;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES) {
            const VkPhysicalDeviceShaderDrawParametersFeatures *features =
                (const VkPhysicalDeviceShaderDrawParametersFeatures *)next;
            /* One structure only, whatever value it carries: the duplicate rule
             * is about the pNext chain, so it is decided before the value is
             * interpreted. Setting the flag only for a true request let a second
             * structure slip through whenever either copy was false, which is
             * the same fail-closed hole the 8/16-bit branches never had. */
            if (saw_draw_parameters) return INVALID;
            saw_draw_parameters = VK_TRUE;
            /* VK_KHR_shader_draw_parameters is how this Vulkan 1.0 profile
             * exposes the 1.1 feature, so a true request is accepted only with
             * the extension enabled; the extension itself is only advertised
             * when the platform supports it, which the loop above enforced. */
            if (!valid_bool(features->shaderDrawParameters)) return INVALID;
            if (features->shaderDrawParameters) {
                if (!draw_parameters) return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features |= PS5VK_FEATURE_SHADER_DRAW_PARAMETERS;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR) {
            if (saw_memory_model) return INVALID;
            saw_memory_model = VK_TRUE;
            const VkPhysicalDeviceVulkanMemoryModelFeaturesKHR *features =
                (const VkPhysicalDeviceVulkanMemoryModelFeaturesKHR *)next;
            if (!valid_bool(features->vulkanMemoryModel) ||
                !valid_bool(features->vulkanMemoryModelDeviceScope) ||
                !valid_bool(features->vulkanMemoryModelAvailabilityVisibilityChains))
                return INVALID;
            if (features->vulkanMemoryModelAvailabilityVisibilityChains)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if (features->vulkanMemoryModelDeviceScope && !features->vulkanMemoryModel)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if (features->vulkanMemoryModel) {
                if (!memory_model_extension ||
                    !(p->platform.supported_features & PS5VK_FEATURE_VULKAN_MEMORY_MODEL))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features |= PS5VK_FEATURE_VULKAN_MEMORY_MODEL;
            }
            if (features->vulkanMemoryModelDeviceScope) {
                if (!(p->platform.supported_features &
                      PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features |= PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR) {
            if (saw_buffer_address) return INVALID;
            saw_buffer_address = VK_TRUE;
            const VkPhysicalDeviceBufferDeviceAddressFeaturesKHR *features =
                (const VkPhysicalDeviceBufferDeviceAddressFeaturesKHR *)next;
            if (!valid_bool(features->bufferDeviceAddress) ||
                !valid_bool(features->bufferDeviceAddressCaptureReplay) ||
                !valid_bool(features->bufferDeviceAddressMultiDevice)) return INVALID;
            if (features->bufferDeviceAddressCaptureReplay ||
                features->bufferDeviceAddressMultiDevice)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if (features->bufferDeviceAddress) {
                if (!buffer_address_extension ||
                    !(p->platform.supported_features &
                      PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features |= PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            if (saw_timeline) return INVALID;
            saw_timeline = VK_TRUE;
            const VkPhysicalDeviceTimelineSemaphoreFeatures *features =
                (const VkPhysicalDeviceTimelineSemaphoreFeatures *)next;
            if (!valid_bool(features->timelineSemaphore)) return INVALID;
            if (features->timelineSemaphore) {
                if (!timeline_extension ||
                    !(p->platform.supported_features_t09 & PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features_t09 |= PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES) {
            if (saw_separate_depth_stencil) return INVALID;
            saw_separate_depth_stencil = VK_TRUE;
            const VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures *features =
                (const VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures *)next;
            if (!valid_bool(features->separateDepthStencilLayouts)) return INVALID;
            if (features->separateDepthStencilLayouts) {
                if (!separate_depth_stencil_extension || !separate_depth_stencil_supported(p))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features_t09 |= PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES) {
            if (saw_demote) return INVALID;
            saw_demote = VK_TRUE;
            const VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures *features =
                (const VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures *)next;
            if (!valid_bool(features->shaderDemoteToHelperInvocation)) return INVALID;
            if (features->shaderDemoteToHelperInvocation) {
                if (!demote_extension)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features_t09 |= PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES) {
            if (saw_terminate) return INVALID;
            saw_terminate = VK_TRUE;
            const VkPhysicalDeviceShaderTerminateInvocationFeatures *features =
                (const VkPhysicalDeviceShaderTerminateInvocationFeatures *)next;
            if (!valid_bool(features->shaderTerminateInvocation)) return INVALID;
            if (features->shaderTerminateInvocation) {
                if (!terminate_extension)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features_t09 |= PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION;
            }
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES) {
            if (saw_maintenance4) return INVALID;
            saw_maintenance4 = VK_TRUE;
            const VkPhysicalDeviceMaintenance4Features *features =
                (const VkPhysicalDeviceMaintenance4Features *)next;
            if (!valid_bool(features->maintenance4)) return INVALID;
            if (features->maintenance4) {
                if (!maintenance4_extension || !maintenance4_supported(p))
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                enabled_features_t09 |= PS5VK_T09_FEATURE_MAINTENANCE4;
            }
        } else if (next->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT) {
            if (saw_extended_dynamic_state) return INVALID;
            saw_extended_dynamic_state = VK_TRUE;
            const VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *features =
                (const VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *)next;
            if (!valid_bool(features->extendedDynamicState)) return INVALID;
            if (features->extendedDynamicState) {
                if (!extended_dynamic_state_extension) return VK_ERROR_FEATURE_NOT_PRESENT;
                extended_dynamic_state = VK_TRUE;
            }
        } else {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    VkResult core_result = enable_core_features(info->pEnabledFeatures,
        p->platform.supported_features, &enabled_features);
    if (core_result != VK_SUCCESS) return core_result;
    if (enabled_features_t09 & PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2) {
        /* VUID-VkPhysicalDeviceRobustness2FeaturesKHR-robustBufferAccess2-04000 */
        if (!(enabled_features & PS5VK_FEATURE_ROBUST_BUFFER_ACCESS)) return INVALID;
        /* The rounded extent stays inside bound memory only while every
         * descriptor offset is a multiple of the access alignment. */
        const VkPhysicalDeviceLimits *limits = &p->platform.properties.limits;
        if (!limits->minStorageBufferOffsetAlignment ||
            !limits->minUniformBufferOffsetAlignment ||
            limits->minStorageBufferOffsetAlignment % PS5VK_ROBUST_BUFFER_ACCESS_SIZE_ALIGNMENT ||
            limits->minUniformBufferOffsetAlignment % PS5VK_ROBUST_BUFFER_ACCESS_SIZE_ALIGNMENT)
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
    if (pthread_mutex_init(&d->queue_lock, NULL)) {
        p->platform.close(&d->memory); ps5vk_object_free(d, &saved, custom);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    d->physical = p; d->queue.device = d; d->queue.next_serial = 1;
    d->queue.priority_class = q->pQueuePriorities[0] >= 0.5f ? 1u : 0u;
    d->enabled_features = enabled_features;
    d->enabled_features_t09 = enabled_features_t09;
    d->device_group_extension_enabled = group_extension;
    d->timeline_extension_enabled = timeline_extension;
    d->maintenance2_extension_enabled = maintenance2_extension;
    d->create_renderpass2_extension_enabled = create_renderpass2_extension;
    d->swapchain_extension_enabled = swapchain_extension;
    d->memory_requirements2_extension_enabled = memory_requirements2_extension;
    d->dedicated_allocation_extension_enabled = dedicated_allocation_extension;
    d->bind_memory2_extension_enabled = bind_memory2_extension;
    d->maintenance4_extension_enabled = maintenance4_extension;
    d->descriptor_update_template_extension_enabled = descriptor_update_template_extension;
    d->extended_dynamic_state_extension_enabled = extended_dynamic_state_extension;
    d->extended_dynamic_state_enabled = extended_dynamic_state;
    d->platform_features = p->platform.supported_features;
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
    if (swapchain_extension && (!d->graphics_enabled || !d->graphics_submit_enabled ||
                                !d->image_requirements)) {
        if (d->pipeline_cache) ps5vk_compilation_cache_destroy(d->pipeline_cache);
        pthread_mutex_destroy(&d->queue_lock);
        p->platform.close(&d->memory);
        ps5vk_object_free(d, &saved, custom);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    ++i->devices; *out = d;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice d, uint32_t family, uint32_t index, VkQueue *out)
{
    if (out) *out = d && !family && !index ? &d->queue : VK_NULL_HANDLE;
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceGroupPeerMemoryFeaturesKHR(VkDevice d,
    uint32_t heap, uint32_t local, uint32_t remote, VkPeerMemoryFeatureFlags *out)
{
    /* A singleton device group has no peer: valid usage requires distinct
     * local and remote indices. Leave a deterministic zero on invalid calls. */
    if (out) *out = 0;
    (void)d; (void)heap; (void)local; (void)remote;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice d, const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!d) return;
    /* Valid usage requires children destroyed and work completed first. Defend
     * against invalid destruction by retaining ownership, not implicit frees. */
    if (d->swapchains || d->memories || d->buffers || d->buffer_views || d->descriptor_objects ||
        d->pipeline_objects || d->graphics_objects || d->command_pools ||
        d->fences || d->pipeline_caches || d->query_pools || d->semaphores ||
        d->events || d->submission ||
        d->queue.next_serial != d->queue.completed_serial + 1) {
        ++d->lifetime_errors; return;
    }
    if (d->pipeline_cache) {
        ps5vk_compilation_cache_destroy(d->pipeline_cache);
        d->pipeline_cache = NULL;
    }
    d->physical->platform.close(&d->memory);
    pthread_mutex_destroy(&d->queue_lock);
    --d->physical->instance->devices;
    VkAllocationCallbacks a = d->allocator; VkBool32 custom = d->custom_allocator;
    ps5vk_object_free(d, &a, custom);
}

void ps5vk_device_enable_runtime_compiler(VkDevice device)
{
    if (device) device->runtime_compiler_enabled = VK_TRUE;
}
/* Core Vulkan 1.1 physical-device-level names that a 1.1 instance resolves.
 * The spec permits calling them only with a physical device that reports 1.1
 * or later; this device reports 1.0, so they answer exactly as the
 * VK_KHR_get_physical_device_properties2 route does and claim nothing more. */
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p,
    VkPhysicalDeviceFeatures2 *out)
{ vkGetPhysicalDeviceFeatures2KHR(p, out); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice p,
    VkPhysicalDeviceProperties2 *out)
{ vkGetPhysicalDeviceProperties2KHR(p, out); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice p,
    VkFormat format, VkFormatProperties2 *out)
{ vkGetPhysicalDeviceFormatProperties2KHR(p, format, out); }
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice p,
    const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out)
{ return vkGetPhysicalDeviceImageFormatProperties2KHR(p, info, out); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice p,
    uint32_t *count, VkQueueFamilyProperties2 *out)
{ vkGetPhysicalDeviceQueueFamilyProperties2KHR(p, count, out); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice p,
    VkPhysicalDeviceMemoryProperties2 *out)
{ vkGetPhysicalDeviceMemoryProperties2KHR(p, out); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice p,
    const VkPhysicalDeviceSparseImageFormatInfo2 *info, uint32_t *count,
    VkSparseImageFormatProperties2 *out)
{ vkGetPhysicalDeviceSparseImageFormatProperties2KHR(p, info, count, out); }
/* No external memory, fence or semaphore handle type is supported: every
 * query reports empty capabilities. */
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice p,
    const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *out)
{
    if (!p || !info || !out) return;
    out->externalMemoryProperties = (VkExternalMemoryProperties){0};
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice p,
    const VkPhysicalDeviceExternalFenceInfo *info, VkExternalFenceProperties *out)
{
    if (!p || !info || !out) return;
    out->exportFromImportedHandleTypes = 0;
    out->compatibleHandleTypes = 0;
    out->externalFenceFeatures = 0;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice p,
    const VkPhysicalDeviceExternalSemaphoreInfo *info, VkExternalSemaphoreProperties *out)
{
    if (!p || !info || !out) return;
    out->exportFromImportedHandleTypes = 0;
    out->compatibleHandleTypes = 0;
    out->externalSemaphoreFeatures = 0;
}
