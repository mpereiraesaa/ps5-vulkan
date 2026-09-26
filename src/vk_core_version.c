#include "vk_core_version.h"
#include "physical_device_profile.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

uint32_t ps5vk_physical_api_version(VkPhysicalDevice p)
{
    return p ? VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(p->platform.properties.apiVersion),
                                   VK_API_VERSION_MINOR(p->platform.properties.apiVersion), 0)
             : VK_API_VERSION_1_0;
}

uint32_t ps5vk_effective_api_version(VkPhysicalDevice p)
{
    const uint32_t device = ps5vk_physical_api_version(p);
    const uint32_t instance = p && p->instance ? p->instance->api_version : VK_API_VERSION_1_0;
    return device < instance ? device : instance;
}

/* ---- features: projections of the per-extension answers ---------------- */

struct extension_features {
    VkPhysicalDevice16BitStorageFeatures storage16;
    VkPhysicalDevice8BitStorageFeatures storage8;
    VkPhysicalDeviceMultiviewFeatures multiview;
    VkPhysicalDeviceProtectedMemoryFeatures protected_memory;
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters;
    VkPhysicalDeviceVulkanMemoryModelFeatures memory_model;
    VkPhysicalDeviceBufferDeviceAddressFeatures address;
    VkPhysicalDeviceUniformBufferStandardLayoutFeatures ubo_layout;
    VkPhysicalDeviceHostQueryResetFeatures query_reset;
    VkPhysicalDeviceImagelessFramebufferFeatures imageless;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline;
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures depth_stencil;
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures demote;
    VkPhysicalDeviceShaderTerminateInvocationFeatures terminate;
    VkPhysicalDeviceMaintenance4Features maintenance4;
    VkPhysicalDeviceSynchronization2Features synchronization2;
    VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering;
    VkPhysicalDeviceFeatures2 core;
};

static void query_extension_features(VkPhysicalDevice p, struct extension_features *f)
{
    memset(f, 0, sizeof(*f));
    struct { VkStructureType type; void *object; } chain[] = {
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &f->storage16},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, &f->storage8},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, &f->multiview},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, &f->protected_memory},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, &f->draw_parameters},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, &f->memory_model},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, &f->address},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, &f->ubo_layout},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, &f->query_reset},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, &f->imageless},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, &f->timeline},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, &f->depth_stencil},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, &f->demote},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, &f->terminate},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, &f->maintenance4},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, &f->synchronization2},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, &f->dynamic_rendering},
    };
    void *next = NULL;
    for (size_t n = sizeof(chain) / sizeof(chain[0]); n-- > 0;) {
        VkBaseOutStructure *s = chain[n].object;
        s->sType = chain[n].type;
        s->pNext = next;
        next = s;
    }
    f->core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f->core.pNext = next;
    vkGetPhysicalDeviceFeatures2KHR(p, &f->core);
}

static void fill_vulkan11_features(VkPhysicalDevice p, VkPhysicalDeviceVulkan11Features *out)
{
    struct extension_features f;
    query_extension_features(p, &f);
    VkBaseOutStructure *keep = (VkBaseOutStructure *)out->pNext;
    memset(out, 0, sizeof(*out));
    out->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    out->pNext = (void *)keep;
    out->storageBuffer16BitAccess = f.storage16.storageBuffer16BitAccess;
    out->uniformAndStorageBuffer16BitAccess = f.storage16.uniformAndStorageBuffer16BitAccess;
    out->storagePushConstant16 = f.storage16.storagePushConstant16;
    out->storageInputOutput16 = f.storage16.storageInputOutput16;
    out->multiview = f.multiview.multiview;
    out->multiviewGeometryShader = f.multiview.multiviewGeometryShader;
    out->multiviewTessellationShader = f.multiview.multiviewTessellationShader;
    out->protectedMemory = f.protected_memory.protectedMemory;
    out->shaderDrawParameters = f.draw_parameters.shaderDrawParameters;
    /* variablePointers and samplerYcbcrConversion stay false. */
}

static void fill_vulkan12_features(VkPhysicalDevice p, VkPhysicalDeviceVulkan12Features *out)
{
    struct extension_features f;
    query_extension_features(p, &f);
    VkBaseOutStructure *keep = (VkBaseOutStructure *)out->pNext;
    memset(out, 0, sizeof(*out));
    out->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    out->pNext = (void *)keep;
    /* VK_KHR_sampler_mirror_clamp_to_edge has no feature structure. */
    out->samplerMirrorClampToEdge = !!(p->platform.supported_features_t09 &
                                       PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE);
    out->storageBuffer8BitAccess = f.storage8.storageBuffer8BitAccess;
    out->uniformAndStorageBuffer8BitAccess = f.storage8.uniformAndStorageBuffer8BitAccess;
    out->storagePushConstant8 = f.storage8.storagePushConstant8;
    out->imagelessFramebuffer = f.imageless.imagelessFramebuffer;
    out->uniformBufferStandardLayout = f.ubo_layout.uniformBufferStandardLayout;
    out->separateDepthStencilLayouts = f.depth_stencil.separateDepthStencilLayouts;
    out->hostQueryReset = f.query_reset.hostQueryReset;
    out->timelineSemaphore = f.timeline.timelineSemaphore;
    out->bufferDeviceAddress = f.address.bufferDeviceAddress;
    out->bufferDeviceAddressCaptureReplay = f.address.bufferDeviceAddressCaptureReplay;
    out->bufferDeviceAddressMultiDevice = f.address.bufferDeviceAddressMultiDevice;
    out->vulkanMemoryModel = f.memory_model.vulkanMemoryModel;
    out->vulkanMemoryModelDeviceScope = f.memory_model.vulkanMemoryModelDeviceScope;
    out->vulkanMemoryModelAvailabilityVisibilityChains =
        f.memory_model.vulkanMemoryModelAvailabilityVisibilityChains;
    /* Subgroup, descriptor-indexing, float16/int8, int64-atomic, scalar
     * layout, minmax, draw-indirect-count and viewport-layer members stay
     * false: no such route is reported. */
}

static void fill_vulkan13_features(VkPhysicalDevice p, VkPhysicalDeviceVulkan13Features *out)
{
    struct extension_features f;
    query_extension_features(p, &f);
    VkBaseOutStructure *keep = (VkBaseOutStructure *)out->pNext;
    memset(out, 0, sizeof(*out));
    out->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    out->pNext = (void *)keep;
    out->shaderDemoteToHelperInvocation = f.demote.shaderDemoteToHelperInvocation;
    out->shaderTerminateInvocation = f.terminate.shaderTerminateInvocation;
    out->maintenance4 = f.maintenance4.maintenance4;
    out->synchronization2 = f.synchronization2.synchronization2;
    out->dynamicRendering = f.dynamic_rendering.dynamicRendering;
}

int ps5vk_core_version_features(VkPhysicalDevice p, VkBaseOutStructure *next)
{
    if (!p || !next) return 0;
    const uint32_t version = ps5vk_physical_api_version(p);
    switch (next->sType) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
        if (version < VK_API_VERSION_1_2) return 0;
        fill_vulkan11_features(p, (VkPhysicalDeviceVulkan11Features *)next);
        return 1;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        if (version < VK_API_VERSION_1_2) return 0;
        fill_vulkan12_features(p, (VkPhysicalDeviceVulkan12Features *)next);
        return 1;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
        if (version < VK_API_VERSION_1_3) return 0;
        fill_vulkan13_features(p, (VkPhysicalDeviceVulkan13Features *)next);
        return 1;
    default:
        return 0;
    }
}

/* ---- properties ---------------------------------------------------------- */

struct extension_properties {
    VkPhysicalDeviceSubgroupProperties subgroup;
    VkPhysicalDeviceMultiviewProperties multiview;
    VkPhysicalDevicePointClippingProperties point_clipping;
    VkPhysicalDeviceTimelineSemaphoreProperties timeline;
    VkPhysicalDeviceMaintenance4Properties maintenance4;
    VkPhysicalDeviceDepthStencilResolveProperties resolve;
    VkPhysicalDeviceProperties2 core;
};

static void query_extension_properties(VkPhysicalDevice p, struct extension_properties *e)
{
    memset(e, 0, sizeof(*e));
    e->maintenance4.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES;
    e->resolve.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;
    e->maintenance4.pNext = &e->resolve;
    e->timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES;
    e->timeline.pNext = &e->maintenance4;
    e->point_clipping.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES;
    e->point_clipping.pNext = &e->timeline;
    e->multiview.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES;
    e->multiview.pNext = &e->point_clipping;
    e->subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    e->subgroup.pNext = &e->multiview;
    e->core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    e->core.pNext = &e->subgroup;
    vkGetPhysicalDeviceProperties2KHR(p, &e->core);
}

/* Public, deterministic identities: FNV-1a over a tag and the reported
 * vendor/device/driver identity. They identify this driver build family, not
 * a console. */
static void identity_uuid(uint8_t out[VK_UUID_SIZE], const char *tag, VkPhysicalDevice p)
{
    uint64_t a = UINT64_C(0xcbf29ce484222325), b = UINT64_C(0x84222325cbf29ce4);
    for (const char *c = tag; *c; ++c) {
        ps5vk_profile_mix(&a, (uint8_t)*c);
        ps5vk_profile_mix(&b, (uint8_t)*c ^ 0xa5u);
    }
    const VkPhysicalDeviceProperties *props = &p->platform.properties;
    ps5vk_profile_mix(&a, props->vendorID);  ps5vk_profile_mix(&b, props->deviceID);
    ps5vk_profile_mix(&a, props->driverVersion); ps5vk_profile_mix(&b, PS5VK_GFX_TARGET);
    for (unsigned i = 0; i < 8; ++i) {
        out[i] = (uint8_t)(a >> (8 * i));
        out[8 + i] = (uint8_t)(b >> (8 * i));
    }
}

static void fill_id(VkPhysicalDevice p, uint8_t device_uuid[VK_UUID_SIZE],
                    uint8_t driver_uuid[VK_UUID_SIZE], uint8_t luid[VK_LUID_SIZE],
                    uint32_t *node_mask, VkBool32 *luid_valid)
{
    identity_uuid(device_uuid, "ps5vk-device", p);
    identity_uuid(driver_uuid, "ps5vk-driver", p);
    memset(luid, 0, VK_LUID_SIZE);
    *node_mask = 0;
    *luid_valid = VK_FALSE;
}

static VkDeviceSize max_memory_allocation(VkPhysicalDevice p)
{
    return p->platform.max_allocation;
}

static void fill_vulkan11_properties(VkPhysicalDevice p, VkPhysicalDeviceVulkan11Properties *out)
{
    struct extension_properties e;
    query_extension_properties(p, &e);
    void *keep = out->pNext;
    memset(out, 0, sizeof(*out));
    out->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
    out->pNext = keep;
    fill_id(p, out->deviceUUID, out->driverUUID, out->deviceLUID, &out->deviceNodeMask,
            &out->deviceLUIDValid);
    out->subgroupSize = e.subgroup.subgroupSize;
    out->subgroupSupportedStages = e.subgroup.supportedStages;
    out->subgroupSupportedOperations = e.subgroup.supportedOperations;
    out->subgroupQuadOperationsInAllStages = e.subgroup.quadOperationsInAllStages;
    out->pointClippingBehavior = e.point_clipping.pointClippingBehavior;
    out->maxMultiviewViewCount = e.multiview.maxMultiviewViewCount;
    out->maxMultiviewInstanceIndex = e.multiview.maxMultiviewInstanceIndex;
    out->protectedNoFault = VK_FALSE;
    /* The descriptor table's real capacity, even though it is below the
     * 1024 the version requires: the contract gate, not this value, decides
     * whether the version may be reported. */
    out->maxPerSetDescriptors = PS5VK_MAX_DESCRIPTORS;
    out->maxMemoryAllocationSize = max_memory_allocation(p);
}

static void fill_driver(char name[VK_MAX_DRIVER_NAME_SIZE], char info[VK_MAX_DRIVER_INFO_SIZE],
                        VkConformanceVersion *conformance, VkDriverId *id)
{
    /* No registered VkDriverId identifies this driver; 0 is not a claim to
     * be any registered one. Never submitted for conformance: 0.0.0.0. */
    *id = (VkDriverId)0;
    snprintf(name, VK_MAX_DRIVER_NAME_SIZE, "ps5vk");
    snprintf(info, VK_MAX_DRIVER_INFO_SIZE, "ps5vk %u (gfx%u)", PS5VK_DRIVER_VERSION,
             PS5VK_GFX_TARGET);
    memset(conformance, 0, sizeof(*conformance));
}

static void fill_vulkan12_properties(VkPhysicalDevice p, VkPhysicalDeviceVulkan12Properties *out)
{
    struct extension_properties e;
    query_extension_properties(p, &e);
    void *keep = out->pNext;
    memset(out, 0, sizeof(*out));
    out->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    out->pNext = keep;
    fill_driver(out->driverName, out->driverInfo, &out->conformanceVersion, &out->driverID);
    /* No float-control guarantee is measured: every preserve/flush/rounding
     * member stays false, which also means no independent control. */
    out->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
    out->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
    out->supportedDepthResolveModes = e.resolve.supportedDepthResolveModes;
    out->supportedStencilResolveModes = e.resolve.supportedStencilResolveModes;
    out->independentResolveNone = e.resolve.independentResolveNone;
    out->independentResolve = e.resolve.independentResolve;
    /* Descriptor indexing and minmax filtering remain unreported. */
    out->maxTimelineSemaphoreValueDifference = e.timeline.maxTimelineSemaphoreValueDifference;
}

static void fill_vulkan13_properties(VkPhysicalDevice p, VkPhysicalDeviceVulkan13Properties *out)
{
    struct extension_properties e;
    query_extension_properties(p, &e);
    void *keep = out->pNext;
    memset(out, 0, sizeof(*out));
    out->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
    out->pNext = keep;
    if (e.subgroup.supportedStages) {
        /* One fixed subgroup size: the reported one is both bounds. */
        out->minSubgroupSize = e.subgroup.subgroupSize;
        out->maxSubgroupSize = e.subgroup.subgroupSize;
        out->maxComputeWorkgroupSubgroups = e.subgroup.subgroupSize ?
            p->platform.properties.limits.maxComputeWorkGroupInvocations /
                e.subgroup.subgroupSize : 0;
    }
    /* Texel-buffer offsets follow the 1.0 alignment; no single-texel rule. */
    out->storageTexelBufferOffsetAlignmentBytes =
        p->platform.properties.limits.minTexelBufferOffsetAlignment;
    out->uniformTexelBufferOffsetAlignmentBytes =
        p->platform.properties.limits.minTexelBufferOffsetAlignment;
    out->maxBufferSize = e.maintenance4.maxBufferSize;
    /* Inline uniform limits and integer dot product acceleration stay zero. */
}

int ps5vk_core_version_properties(VkPhysicalDevice p, VkBaseOutStructure *next)
{
    if (!p || !next) return 0;
    const uint32_t version = ps5vk_physical_api_version(p);
    switch (next->sType) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
        if (version < VK_API_VERSION_1_2) return 0;
        fill_vulkan11_properties(p, (VkPhysicalDeviceVulkan11Properties *)next);
        return 1;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
        if (version < VK_API_VERSION_1_2) return 0;
        fill_vulkan12_properties(p, (VkPhysicalDeviceVulkan12Properties *)next);
        return 1;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES:
        if (version < VK_API_VERSION_1_3) return 0;
        fill_vulkan13_properties(p, (VkPhysicalDeviceVulkan13Properties *)next);
        return 1;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: {
        if (version < VK_API_VERSION_1_1) return 0;
        VkPhysicalDeviceIDProperties *out = (VkPhysicalDeviceIDProperties *)next;
        fill_id(p, out->deviceUUID, out->driverUUID, out->deviceLUID, &out->deviceNodeMask,
                &out->deviceLUIDValid);
        return 1;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES: {
        if (version < VK_API_VERSION_1_1) return 0;
        VkPhysicalDeviceMaintenance3Properties *out = (VkPhysicalDeviceMaintenance3Properties *)next;
        out->maxPerSetDescriptors = PS5VK_MAX_DESCRIPTORS;
        out->maxMemoryAllocationSize = max_memory_allocation(p);
        return 1;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES:
        if (version < VK_API_VERSION_1_1) return 0;
        ((VkPhysicalDeviceProtectedMemoryProperties *)next)->protectedNoFault = VK_FALSE;
        return 1;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
        if (version < VK_API_VERSION_1_2) return 0;
        VkPhysicalDeviceDriverProperties *out = (VkPhysicalDeviceDriverProperties *)next;
        fill_driver(out->driverName, out->driverInfo, &out->conformanceVersion, &out->driverID);
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- device creation ----------------------------------------------------- */

/* An aggregate and its constituent feature structures cannot coexist in a
 * device-create chain, even when every member is false. Check the whole chain
 * before processing requests, so order never changes the result. */
VkResult ps5vk_core_version_validate_chain(const VkBaseInStructure *chain)
{
    uint32_t aggregates = 0, individual = 0;
    for (const VkBaseInStructure *s = chain; s; s = s->pNext) {
        uint32_t aggregate = 0, member = 0;
        switch (s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: aggregate = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: aggregate = 2; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: aggregate = 4; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
            member = 1; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
            member = 2; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES:
            member = 4; break;
        default: break;
        }
        if (aggregate & aggregates) return VK_ERROR_UNKNOWN;
        aggregates |= aggregate;
        individual |= member;
    }
    return aggregates & individual ? VK_ERROR_UNKNOWN : VK_SUCCESS;
}

struct core_enable {
    VkStructureType type;
    size_t offset;
    uint32_t bit;      /* enabled_features bit, or 0 */
    uint32_t bit_t09;  /* enabled_features_t09 bit, or 0 */
};

#define V11(member, bit, t09) {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, \
    offsetof(VkPhysicalDeviceVulkan11Features, member), bit, t09}
#define V12(member, bit, t09) {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, \
    offsetof(VkPhysicalDeviceVulkan12Features, member), bit, t09}
#define V13(member, bit, t09) {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, \
    offsetof(VkPhysicalDeviceVulkan13Features, member), bit, t09}
/* The same enabled bits the per-extension structures set. A reported member
 * without an entry enables no gate of its own. */
static const struct core_enable core_enables[] = {
    V11(storageBuffer16BitAccess, PS5VK_FEATURE_STORAGE_BUFFER_16BIT, 0),
    V11(multiview, PS5VK_FEATURE_MULTIVIEW, 0),
    V11(shaderDrawParameters, PS5VK_FEATURE_SHADER_DRAW_PARAMETERS, 0),
    V12(samplerMirrorClampToEdge, 0, PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE),
    V12(storageBuffer8BitAccess, PS5VK_FEATURE_STORAGE_BUFFER_8BIT, 0),
    V12(imagelessFramebuffer, 0, PS5VK_T09_FEATURE_IMAGELESS_FRAMEBUFFER),
    V12(uniformBufferStandardLayout, PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT, 0),
    V12(separateDepthStencilLayouts, 0, PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS),
    V12(hostQueryReset, 0, PS5VK_T09_FEATURE_HOST_QUERY_RESET),
    V12(timelineSemaphore, 0, PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE),
    V12(bufferDeviceAddress, PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS, 0),
    V12(vulkanMemoryModel, PS5VK_FEATURE_VULKAN_MEMORY_MODEL, 0),
    V12(vulkanMemoryModelDeviceScope, PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE, 0),
    V13(shaderDemoteToHelperInvocation, 0, PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION),
    V13(shaderTerminateInvocation, 0, PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION),
    V13(maintenance4, 0, PS5VK_T09_FEATURE_MAINTENANCE4),
    V13(synchronization2, 0, PS5VK_T09_FEATURE_SYNCHRONIZATION2),
    V13(dynamicRendering, 0, PS5VK_T09_FEATURE_DYNAMIC_RENDERING),
};
#undef V11
#undef V12
#undef V13

VkResult ps5vk_core_version_enable(VkPhysicalDevice p, const VkBaseInStructure *next,
                                   uint32_t *seen, uint32_t *enabled,
                                   uint32_t *enabled_t09)
{
    if (!p || !next || !seen || !enabled || !enabled_t09) return VK_ERROR_FEATURE_NOT_PRESENT;
    const uint32_t version = ps5vk_effective_api_version(p);
    size_t size, first;
    uint32_t minimum, flag;
    union {
        VkPhysicalDeviceVulkan11Features v11;
        VkPhysicalDeviceVulkan12Features v12;
        VkPhysicalDeviceVulkan13Features v13;
    } reported;
    memset(&reported, 0, sizeof(reported));
    switch (next->sType) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
        size = sizeof(VkPhysicalDeviceVulkan11Features);
        first = offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess);
        minimum = VK_API_VERSION_1_2; flag = 1u;
        fill_vulkan11_features(p, &reported.v11);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        size = sizeof(VkPhysicalDeviceVulkan12Features);
        first = offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge);
        minimum = VK_API_VERSION_1_2; flag = 2u;
        fill_vulkan12_features(p, &reported.v12);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
        size = sizeof(VkPhysicalDeviceVulkan13Features);
        first = offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess);
        minimum = VK_API_VERSION_1_3; flag = 4u;
        fill_vulkan13_features(p, &reported.v13);
        break;
    default:
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (version < minimum) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (*seen & flag) return VK_ERROR_UNKNOWN;
    *seen |= flag;
    const unsigned char *requested = (const unsigned char *)next;
    const unsigned char *supported = (const unsigned char *)&reported;
    uint32_t add = 0, add_t09 = 0;
    for (size_t offset = first; offset + sizeof(VkBool32) <= size; offset += sizeof(VkBool32)) {
        VkBool32 want, have;
        memcpy(&want, requested + offset, sizeof(want));
        memcpy(&have, supported + offset, sizeof(have));
        if (want != VK_FALSE && want != VK_TRUE) return VK_ERROR_UNKNOWN;
        if (!want) continue;
        if (!have) return VK_ERROR_FEATURE_NOT_PRESENT;
        for (size_t n = 0; n < sizeof(core_enables) / sizeof(core_enables[0]); ++n)
            if (core_enables[n].type == next->sType && core_enables[n].offset == offset) {
                add |= core_enables[n].bit;
                add_t09 |= core_enables[n].bit_t09;
            }
    }
    *enabled |= add;
    *enabled_t09 |= add_t09;
    return VK_SUCCESS;
}

void ps5vk_core_version_promote(VkDevice d)
{
    if (!d) return;
    const uint32_t version = ps5vk_effective_api_version(d->physical);
    if (version >= VK_API_VERSION_1_1) {
        d->device_group_extension_enabled = VK_TRUE;
        d->maintenance1_extension_enabled = VK_TRUE;
        d->maintenance2_extension_enabled = VK_TRUE;
        d->memory_requirements2_extension_enabled = VK_TRUE;
        d->dedicated_allocation_extension_enabled = VK_TRUE;
        d->bind_memory2_extension_enabled = VK_TRUE;
        d->descriptor_update_template_extension_enabled = VK_TRUE;
    }
    if (version >= VK_API_VERSION_1_2) {
        d->timeline_extension_enabled = VK_TRUE;
        d->create_renderpass2_extension_enabled = VK_TRUE;
        d->image_format_list_extension_enabled = VK_TRUE;
        d->depth_stencil_resolve_extension_enabled = VK_TRUE;
    }
    if (version >= VK_API_VERSION_1_3) {
        d->maintenance4_extension_enabled = VK_TRUE;
        d->copy_commands2_extension_enabled = VK_TRUE;
        d->extended_dynamic_state_extension_enabled = VK_TRUE;
        /* Unlike the optional extension feature, this state is core in 1.3. */
        d->extended_dynamic_state_enabled =
            !!(d->physical->platform.supported_features_t09 & PS5VK_T09_FEATURE_EXTENDED_DYNAMIC_STATE);
        d->synchronization2_extension_enabled = VK_TRUE;
        d->dynamic_rendering_extension_enabled = VK_TRUE;
        d->dynamic_rendering_enabled |=
            !!(d->enabled_features_t09 & PS5VK_T09_FEATURE_DYNAMIC_RENDERING);
    }
}
