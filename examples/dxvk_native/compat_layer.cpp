/* DIAGNOSTIC compatibility translation layer (diagnostic-compat variant only).
 *
 * Pinned DXVK 2.6.2 assumes a Vulkan 1.3 device: it queries and enables
 * features and properties only through VkPhysicalDeviceVulkan11/12/13*
 * aggregate structures. ps5vk reports a Vulkan 1.0 device whose optional
 * functionality is exposed through per-extension structures. This layer, which
 * sits between DXVK and ps5vk in the payload's Vulkan trace, translates:
 *
 *  - feature/property queries: aggregate structures are removed from the
 *    chain passed to ps5vk and filled ONLY from the per-extension structures
 *    of extensions ps5vk enumerates (or, for features defined by extension
 *    presence alone, from that presence). Nothing ps5vk does not report is
 *    ever set.
 *  - vkCreateDevice: enabled aggregate bits become the matching device
 *    extensions plus per-extension feature structures. A requested bit with
 *    no ps5vk route is a translation refusal: every one is logged with its
 *    exact name and device creation fails with VK_ERROR_FEATURE_NOT_PRESENT.
 *
 * Every translation is logged. A run of this layer is never an unmodified
 * DXVK run. */
#include "build_identity.h"
#include "telemetry.h"
#include "compat_layer.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#if DXVK_NATIVE_COMPAT_LAYER

namespace {

enum Aggregate { VK11, VK12, VK13 };
const char *const kAggregateNames[] = {"vk11", "vk12", "vk13"};

/* One aggregate member and where ps5vk reports it. sType == 0 means the
 * feature is defined by the presence of the extension alone. */
struct Route {
    Aggregate aggregate;
    const char *member;
    size_t aggregate_offset;
    size_t size;
    const char *extension; /* nullptr: no ps5vk route (core-only member) */
    VkStructureType stype;
    size_t struct_size;
    size_t struct_offset;
};

#define AGG_TYPE_VK11 VkPhysicalDeviceVulkan11Features
#define AGG_TYPE_VK12 VkPhysicalDeviceVulkan12Features
#define AGG_TYPE_VK13 VkPhysicalDeviceVulkan13Features
#define FEAT(agg, member, ext, type, stype) \
    {agg, #member, offsetof(AGG_TYPE_##agg, member), sizeof(VkBool32), ext, stype, \
     sizeof(type), offsetof(type, member)}
#define IMPLIED(agg, member, ext) \
    {agg, #member, offsetof(AGG_TYPE_##agg, member), sizeof(VkBool32), ext, \
     VkStructureType(0), 0, 0}
#define NOROUTE(agg, member) \
    {agg, #member, offsetof(AGG_TYPE_##agg, member), sizeof(VkBool32), nullptr, \
     VkStructureType(0), 0, 0}

#define S16 VkPhysicalDevice16BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES
#define SMV VkPhysicalDeviceMultiviewFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES
#define SVP VkPhysicalDeviceVariablePointersFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES
#define SYC VkPhysicalDeviceSamplerYcbcrConversionFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES
#define S8 VkPhysicalDevice8BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES
#define SA64 VkPhysicalDeviceShaderAtomicInt64Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES
#define SF16 VkPhysicalDeviceShaderFloat16Int8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES
#define SDI VkPhysicalDeviceDescriptorIndexingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES
#define SSB VkPhysicalDeviceScalarBlockLayoutFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES
#define SIF VkPhysicalDeviceImagelessFramebufferFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES
#define SUB VkPhysicalDeviceUniformBufferStandardLayoutFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES
#define SSET VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES
#define SDS VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES
#define SHQ VkPhysicalDeviceHostQueryResetFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES
#define STL VkPhysicalDeviceTimelineSemaphoreFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES
#define SBDA VkPhysicalDeviceBufferDeviceAddressFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES
#define SMM VkPhysicalDeviceVulkanMemoryModelFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES
#define SIR VkPhysicalDeviceImageRobustnessFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES
#define SIUB VkPhysicalDeviceInlineUniformBlockFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES
#define SPCC VkPhysicalDevicePipelineCreationCacheControlFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES
#define SPD VkPhysicalDevicePrivateDataFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES
#define SDEM VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES
#define STI VkPhysicalDeviceShaderTerminateInvocationFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES
#define SSSC VkPhysicalDeviceSubgroupSizeControlFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES
#define SS2 VkPhysicalDeviceSynchronization2Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES
#define SASTC VkPhysicalDeviceTextureCompressionASTCHDRFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES
#define SZI VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES
#define SDR VkPhysicalDeviceDynamicRenderingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES
#define SIDP VkPhysicalDeviceShaderIntegerDotProductFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES
#define SM4 VkPhysicalDeviceMaintenance4Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES

/* FEAT_ expands the struct/sType pair macros above. */
#define FEAT_(agg, member, ext, pair) FEAT_X(agg, member, ext, pair)
#define FEAT_X(agg, member, ext, type, stype) FEAT(agg, member, ext, type, stype)

const Route kFeatureRoutes[] = {
    FEAT_(VK11, storageBuffer16BitAccess, "VK_KHR_16bit_storage", S16),
    FEAT_(VK11, uniformAndStorageBuffer16BitAccess, "VK_KHR_16bit_storage", S16),
    FEAT_(VK11, storagePushConstant16, "VK_KHR_16bit_storage", S16),
    FEAT_(VK11, storageInputOutput16, "VK_KHR_16bit_storage", S16),
    FEAT_(VK11, multiview, "VK_KHR_multiview", SMV),
    FEAT_(VK11, multiviewGeometryShader, "VK_KHR_multiview", SMV),
    FEAT_(VK11, multiviewTessellationShader, "VK_KHR_multiview", SMV),
    FEAT_(VK11, variablePointersStorageBuffer, "VK_KHR_variable_pointers", SVP),
    FEAT_(VK11, variablePointers, "VK_KHR_variable_pointers", SVP),
    NOROUTE(VK11, protectedMemory),
    FEAT_(VK11, samplerYcbcrConversion, "VK_KHR_sampler_ycbcr_conversion", SYC),
    IMPLIED(VK11, shaderDrawParameters, "VK_KHR_shader_draw_parameters"),

    IMPLIED(VK12, samplerMirrorClampToEdge, "VK_KHR_sampler_mirror_clamp_to_edge"),
    IMPLIED(VK12, drawIndirectCount, "VK_KHR_draw_indirect_count"),
    FEAT_(VK12, storageBuffer8BitAccess, "VK_KHR_8bit_storage", S8),
    FEAT_(VK12, uniformAndStorageBuffer8BitAccess, "VK_KHR_8bit_storage", S8),
    FEAT_(VK12, storagePushConstant8, "VK_KHR_8bit_storage", S8),
    FEAT_(VK12, shaderBufferInt64Atomics, "VK_KHR_shader_atomic_int64", SA64),
    FEAT_(VK12, shaderSharedInt64Atomics, "VK_KHR_shader_atomic_int64", SA64),
    FEAT_(VK12, shaderFloat16, "VK_KHR_shader_float16_int8", SF16),
    FEAT_(VK12, shaderInt8, "VK_KHR_shader_float16_int8", SF16),
    IMPLIED(VK12, descriptorIndexing, "VK_EXT_descriptor_indexing"),
    FEAT_(VK12, shaderInputAttachmentArrayDynamicIndexing, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, shaderUniformTexelBufferArrayDynamicIndexing, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, shaderStorageTexelBufferArrayDynamicIndexing, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, shaderUniformBufferArrayNonUniformIndexing, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, shaderSampledImageArrayNonUniformIndexing, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, shaderStorageBufferArrayNonUniformIndexing, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, shaderStorageImageArrayNonUniformIndexing, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, shaderInputAttachmentArrayNonUniformIndexing, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, shaderUniformTexelBufferArrayNonUniformIndexing, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, shaderStorageTexelBufferArrayNonUniformIndexing, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, descriptorBindingUniformBufferUpdateAfterBind, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, descriptorBindingSampledImageUpdateAfterBind, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, descriptorBindingStorageImageUpdateAfterBind, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, descriptorBindingStorageBufferUpdateAfterBind, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, descriptorBindingUniformTexelBufferUpdateAfterBind, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, descriptorBindingStorageTexelBufferUpdateAfterBind, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, descriptorBindingUpdateUnusedWhilePending, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, descriptorBindingPartiallyBound, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, descriptorBindingVariableDescriptorCount, "VK_EXT_descriptor_indexing", SDI),
    FEAT_(VK12, runtimeDescriptorArray, "VK_EXT_descriptor_indexing", SDI),
    IMPLIED(VK12, samplerFilterMinmax, "VK_EXT_sampler_filter_minmax"),
    FEAT_(VK12, scalarBlockLayout, "VK_EXT_scalar_block_layout", SSB),
    FEAT_(VK12, imagelessFramebuffer, "VK_KHR_imageless_framebuffer", SIF),
    FEAT_(VK12, uniformBufferStandardLayout, "VK_KHR_uniform_buffer_standard_layout", SUB),
    FEAT_(VK12, shaderSubgroupExtendedTypes, "VK_KHR_shader_subgroup_extended_types", SSET),
    FEAT_(VK12, separateDepthStencilLayouts, "VK_KHR_separate_depth_stencil_layouts", SDS),
    FEAT_(VK12, hostQueryReset, "VK_EXT_host_query_reset", SHQ),
    FEAT_(VK12, timelineSemaphore, "VK_KHR_timeline_semaphore", STL),
    FEAT_(VK12, bufferDeviceAddress, "VK_KHR_buffer_device_address", SBDA),
    FEAT_(VK12, bufferDeviceAddressCaptureReplay, "VK_KHR_buffer_device_address", SBDA),
    FEAT_(VK12, bufferDeviceAddressMultiDevice, "VK_KHR_buffer_device_address", SBDA),
    FEAT_(VK12, vulkanMemoryModel, "VK_KHR_vulkan_memory_model", SMM),
    FEAT_(VK12, vulkanMemoryModelDeviceScope, "VK_KHR_vulkan_memory_model", SMM),
    FEAT_(VK12, vulkanMemoryModelAvailabilityVisibilityChains, "VK_KHR_vulkan_memory_model", SMM),
    IMPLIED(VK12, shaderOutputViewportIndex, "VK_EXT_shader_viewport_index_layer"),
    IMPLIED(VK12, shaderOutputLayer, "VK_EXT_shader_viewport_index_layer"),
    NOROUTE(VK12, subgroupBroadcastDynamicId),

    FEAT_(VK13, robustImageAccess, "VK_EXT_image_robustness", SIR),
    FEAT_(VK13, inlineUniformBlock, "VK_EXT_inline_uniform_block", SIUB),
    FEAT_(VK13, descriptorBindingInlineUniformBlockUpdateAfterBind, "VK_EXT_inline_uniform_block", SIUB),
    FEAT_(VK13, pipelineCreationCacheControl, "VK_EXT_pipeline_creation_cache_control", SPCC),
    FEAT_(VK13, privateData, "VK_EXT_private_data", SPD),
    FEAT_(VK13, shaderDemoteToHelperInvocation, "VK_EXT_shader_demote_to_helper_invocation", SDEM),
    FEAT_(VK13, shaderTerminateInvocation, "VK_KHR_shader_terminate_invocation", STI),
    FEAT_(VK13, subgroupSizeControl, "VK_EXT_subgroup_size_control", SSSC),
    FEAT_(VK13, computeFullSubgroups, "VK_EXT_subgroup_size_control", SSSC),
    FEAT_(VK13, synchronization2, "VK_KHR_synchronization2", SS2),
    FEAT_(VK13, textureCompressionASTC_HDR, "VK_EXT_texture_compression_astc_hdr", SASTC),
    FEAT_(VK13, shaderZeroInitializeWorkgroupMemory, "VK_KHR_zero_initialize_workgroup_memory", SZI),
    FEAT_(VK13, dynamicRendering, "VK_KHR_dynamic_rendering", SDR),
    FEAT_(VK13, shaderIntegerDotProduct, "VK_KHR_shader_integer_dot_product", SIDP),
    FEAT_(VK13, maintenance4, "VK_KHR_maintenance4", SM4),
};

/* Aggregate property members copied from per-extension property structs. */
#define AGGP_TYPE_VK11 VkPhysicalDeviceVulkan11Properties
#define AGGP_TYPE_VK12 VkPhysicalDeviceVulkan12Properties
#define AGGP_TYPE_VK13 VkPhysicalDeviceVulkan13Properties
#define PROP(agg, member, ext, type, stype) \
    {agg, #member, offsetof(AGGP_TYPE_##agg, member), sizeof(AGGP_TYPE_##agg::member), ext, \
     stype, sizeof(type), offsetof(type, member)}
#define PROP_(agg, member, ext, pair) PROP_X(agg, member, ext, pair)
#define PROP_X(agg, member, ext, type, stype) PROP(agg, member, ext, type, stype)
#define PPC VkPhysicalDevicePointClippingProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES
#define PMV VkPhysicalDeviceMultiviewProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES
#define PM3 VkPhysicalDeviceMaintenance3Properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES
#define PDRV VkPhysicalDeviceDriverProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES
#define PDSR VkPhysicalDeviceDepthStencilResolveProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES
#define PMM VkPhysicalDeviceSamplerFilterMinmaxProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES
#define PTL VkPhysicalDeviceTimelineSemaphoreProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES
#define PSSC VkPhysicalDeviceSubgroupSizeControlProperties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES
#define PM4 VkPhysicalDeviceMaintenance4Properties, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES

const Route kPropertyRoutes[] = {
    PROP_(VK11, pointClippingBehavior, "VK_KHR_maintenance2", PPC),
    PROP_(VK11, maxMultiviewViewCount, "VK_KHR_multiview", PMV),
    PROP_(VK11, maxMultiviewInstanceIndex, "VK_KHR_multiview", PMV),
    PROP_(VK11, maxPerSetDescriptors, "VK_KHR_maintenance3", PM3),
    PROP_(VK11, maxMemoryAllocationSize, "VK_KHR_maintenance3", PM3),
    PROP_(VK12, driverID, "VK_KHR_driver_properties", PDRV),
    PROP_(VK12, driverName, "VK_KHR_driver_properties", PDRV),
    PROP_(VK12, driverInfo, "VK_KHR_driver_properties", PDRV),
    PROP_(VK12, conformanceVersion, "VK_KHR_driver_properties", PDRV),
    PROP_(VK12, supportedDepthResolveModes, "VK_KHR_depth_stencil_resolve", PDSR),
    PROP_(VK12, supportedStencilResolveModes, "VK_KHR_depth_stencil_resolve", PDSR),
    PROP_(VK12, independentResolveNone, "VK_KHR_depth_stencil_resolve", PDSR),
    PROP_(VK12, independentResolve, "VK_KHR_depth_stencil_resolve", PDSR),
    PROP_(VK12, filterMinmaxSingleComponentFormats, "VK_EXT_sampler_filter_minmax", PMM),
    PROP_(VK12, filterMinmaxImageComponentMapping, "VK_EXT_sampler_filter_minmax", PMM),
    PROP_(VK12, maxTimelineSemaphoreValueDifference, "VK_KHR_timeline_semaphore", PTL),
    PROP_(VK13, minSubgroupSize, "VK_EXT_subgroup_size_control", PSSC),
    PROP_(VK13, maxSubgroupSize, "VK_EXT_subgroup_size_control", PSSC),
    PROP_(VK13, maxComputeWorkgroupSubgroups, "VK_EXT_subgroup_size_control", PSSC),
    PROP_(VK13, requiredSubgroupSizeStages, "VK_EXT_subgroup_size_control", PSSC),
    PROP_(VK13, maxBufferSize, "VK_KHR_maintenance4", PM4),
};

bool is_aggregate(VkStructureType type, bool properties, Aggregate *which)
{
    const VkStructureType types[2][3] = {
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES}};
    for (int i = 0; i < 3; ++i) {
        if (types[properties][i] == type) {
            if (which) *which = Aggregate(i);
            return true;
        }
    }
    return false;
}

std::vector<std::string> device_extensions(VkPhysicalDevice physical)
{
    std::vector<std::string> names;
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS)
        return names;
    std::vector<VkExtensionProperties> props(count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, props.data()) < 0)
        return names;
    for (uint32_t i = 0; i < count; ++i) names.emplace_back(props[i].extensionName);
    return names;
}

bool has(const std::vector<std::string> &names, const char *name)
{
    for (const std::string &entry : names)
        if (entry == name) return true;
    return false;
}

/* Zero-initialized per-extension structures, one per sType. */
struct ExtensionStructs {
    std::vector<std::vector<unsigned char>> storage;
    std::vector<VkStructureType> types;

    VkBaseOutStructure *get(VkStructureType type, size_t size) {
        for (size_t i = 0; i < types.size(); ++i)
            if (types[i] == type) return reinterpret_cast<VkBaseOutStructure *>(storage[i].data());
        storage.emplace_back(size, 0);
        types.push_back(type);
        auto *s = reinterpret_cast<VkBaseOutStructure *>(storage.back().data());
        s->sType = type;
        return s;
    }
    /* Link every structure into one chain; returns its head. */
    VkBaseOutStructure *link(VkBaseOutStructure *tail) {
        VkBaseOutStructure *head = tail;
        for (size_t i = storage.size(); i-- > 0;) {
            auto *s = reinterpret_cast<VkBaseOutStructure *>(storage[i].data());
            s->pNext = head;
            head = s;
        }
        return head;
    }
};

/* Remove aggregate structures from a mutable chain, run `call`, restore the
 * original links. Returns the removed aggregates. */
template<typename Call>
void without_aggregates(VkBaseOutStructure *root, bool properties, ExtensionStructs &extra,
                        std::vector<std::pair<Aggregate, VkBaseOutStructure *>> &removed,
                        Call call)
{
    std::vector<std::pair<VkBaseOutStructure *, VkBaseOutStructure *>> saved;
    for (VkBaseOutStructure *s = root; s; s = s->pNext) saved.emplace_back(s, s->pNext);
    VkBaseOutStructure *kept_tail = root;
    for (size_t i = 1; i < saved.size(); ++i) {
        Aggregate which;
        VkBaseOutStructure *s = saved[i].first;
        if (is_aggregate(s->sType, properties, &which)) {
            removed.emplace_back(which, s);
        } else {
            kept_tail->pNext = s;
            kept_tail = s;
        }
    }
    kept_tail->pNext = extra.link(nullptr);
    call();
    for (auto &entry : saved) entry.first->pNext = entry.second;
}

VkBaseOutStructure *find_in_chain(VkBaseOutStructure *root, VkStructureType type)
{
    for (VkBaseOutStructure *s = root ? root->pNext : nullptr; s; s = s->pNext)
        if (s->sType == type) return s;
    return nullptr;
}

void translate_query(VkBaseOutStructure *root, VkPhysicalDevice physical, bool properties,
                     void (*call)(VkPhysicalDevice, void *), const Route *routes, size_t count)
{
    const std::vector<std::string> extensions = device_extensions(physical);
    ExtensionStructs extra;
    /* A per-extension struct DXVK chained itself is queried in place. */
    for (size_t i = 0; i < count; ++i)
        if (routes[i].extension && routes[i].stype && has(extensions, routes[i].extension) &&
            !find_in_chain(root, routes[i].stype))
            extra.get(routes[i].stype, routes[i].struct_size);
    std::vector<std::pair<Aggregate, VkBaseOutStructure *>> removed;
    without_aggregates(root, properties, extra, removed, [&] { call(physical, root); });
    for (auto &entry : removed) {
        unsigned copied = 0, absent = 0;
        std::string missing;
        for (size_t i = 0; i < count; ++i) {
            const Route &route = routes[i];
            if (route.aggregate != entry.first) continue;
            unsigned char *dst = reinterpret_cast<unsigned char *>(entry.second) + route.aggregate_offset;
            std::memset(dst, 0, route.size);
            if (!route.extension || !has(extensions, route.extension)) {
                ++absent;
                continue;
            }
            if (!route.stype) {
                VkBool32 one = VK_TRUE;
                std::memcpy(dst, &one, sizeof(one));
            } else {
                VkBaseOutStructure *own = find_in_chain(root, route.stype);
                const unsigned char *src = reinterpret_cast<const unsigned char *>(
                    own ? own : extra.get(route.stype, route.struct_size)) + route.struct_offset;
                std::memcpy(dst, src, route.size);
            }
            ++copied;
        }
        /* Members without a route stay zero; report the set bits for features. */
        if (!properties) {
            std::string on;
            for (size_t i = 0; i < count; ++i) {
                const Route &route = routes[i];
                if (route.aggregate != entry.first) continue;
                VkBool32 value;
                std::memcpy(&value, reinterpret_cast<unsigned char *>(entry.second) +
                            route.aggregate_offset, sizeof(value));
                if (value) on += std::string(on.empty() ? "" : ",") + route.member;
            }
            dxvk_telemetry_emit("WARN", "DXVK_COMPAT_QUERY kind=features struct=%s routed=%u "
                                "unrouted=%u enabled=%s", kAggregateNames[entry.first], copied,
                                absent, on.empty() ? "none" : on.c_str());
        } else {
            dxvk_telemetry_emit("WARN", "DXVK_COMPAT_QUERY kind=properties struct=%s routed=%u "
                                "unrouted=%u", kAggregateNames[entry.first], copied, absent);
        }
    }
}

PFN_vkGetPhysicalDeviceFeatures2 g_features2;
PFN_vkGetPhysicalDeviceProperties2 g_properties2;

void call_features(VkPhysicalDevice p, void *s)
{ g_features2(p, static_cast<VkPhysicalDeviceFeatures2 *>(s)); }
void call_properties(VkPhysicalDevice p, void *s)
{ g_properties2(p, static_cast<VkPhysicalDeviceProperties2 *>(s)); }

} // namespace

void compat_features2(PFN_vkGetPhysicalDeviceFeatures2 real, VkPhysicalDevice physical,
                      VkPhysicalDeviceFeatures2 *features)
{
    g_features2 = real;
    translate_query(reinterpret_cast<VkBaseOutStructure *>(features), physical, false,
                    call_features, kFeatureRoutes, sizeof(kFeatureRoutes) / sizeof(Route));
}

void compat_properties2(PFN_vkGetPhysicalDeviceProperties2 real, VkPhysicalDevice physical,
                        VkPhysicalDeviceProperties2 *properties)
{
    g_properties2 = real;
    translate_query(reinterpret_cast<VkBaseOutStructure *>(properties), physical, true,
                    call_properties, kPropertyRoutes, sizeof(kPropertyRoutes) / sizeof(Route));
}

VkResult compat_create_device(PFN_vkCreateDevice real, VkPhysicalDevice physical,
                              const VkDeviceCreateInfo *info,
                              const VkAllocationCallbacks *allocator, VkDevice *device)
{
    if (!info) return real(physical, info, allocator, device);
    const std::vector<std::string> supported = device_extensions(physical);
    std::vector<const char *> extensions(info->ppEnabledExtensionNames,
                                         info->ppEnabledExtensionNames + info->enabledExtensionCount);
    ExtensionStructs extra;
    std::vector<std::string> refusals, added, routed;
    auto add_extension = [&](const char *name) {
        for (const char *entry : extensions)
            if (!std::strcmp(entry, name)) return;
        extensions.push_back(name);
        added.emplace_back(name);
    };
    /* DXVK 2.6.2 assumes Vulkan 1.3, so it never enables device extensions
     * promoted to core without a feature bit, yet uses their behaviour (e.g.
     * mutable-format images with VkImageFormatListCreateInfo, *2 commands).
     * Enable every such extension ps5vk enumerates. */
    static const char *const kImplicitCore[] = {
        "VK_KHR_maintenance1", "VK_KHR_maintenance2", "VK_KHR_maintenance3",
        "VK_KHR_multiview", "VK_KHR_image_format_list", "VK_KHR_format_feature_flags2",
        "VK_KHR_get_memory_requirements2", "VK_KHR_dedicated_allocation",
        "VK_KHR_bind_memory2", "VK_KHR_copy_commands2", "VK_KHR_create_renderpass2",
        "VK_KHR_depth_stencil_resolve", "VK_KHR_descriptor_update_template",
        "VK_KHR_storage_buffer_storage_class", "VK_KHR_driver_properties",
        "VK_KHR_shader_float_controls",
    };
    for (const char *name : kImplicitCore)
        if (has(supported, name)) add_extension(name);
    /* Vulkan 1.3 made extendedDynamicState core functionality with no feature
     * bit, so DXVK declares its dynamic states without enabling anything.
     * Route it through VK_EXT_extended_dynamic_state when ps5vk reports it. */
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {};
    eds.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
    bool eds_route = false;
    if (has(supported, "VK_EXT_extended_dynamic_state")) {
        VkPhysicalDeviceFeatures2 query = {};
        query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        query.pNext = &eds;
        vkGetPhysicalDeviceFeatures2KHR(physical, &query);
        eds.pNext = nullptr;
        if (eds.extendedDynamicState) {
            add_extension("VK_EXT_extended_dynamic_state");
            eds_route = true;
        }
    }
    /* Existing per-extension structs DXVK chained itself are kept as is. */
    std::vector<std::pair<VkBaseOutStructure *, VkBaseOutStructure *>> saved;
    auto *root = reinterpret_cast<VkBaseOutStructure *>(const_cast<VkDeviceCreateInfo *>(info));
    for (VkBaseOutStructure *s = root; s; s = s->pNext) saved.emplace_back(s, s->pNext);
    VkBaseOutStructure *kept_tail = root;
    const size_t count = sizeof(kFeatureRoutes) / sizeof(Route);
    for (size_t i = 1; i < saved.size(); ++i) {
        Aggregate which;
        VkBaseOutStructure *s = saved[i].first;
        if (!is_aggregate(s->sType, false, &which)) {
            kept_tail->pNext = s;
            kept_tail = s;
            continue;
        }
        for (size_t r = 0; r < count; ++r) {
            const Route &route = kFeatureRoutes[r];
            if (route.aggregate != which) continue;
            VkBool32 value;
            std::memcpy(&value, reinterpret_cast<unsigned char *>(s) + route.aggregate_offset,
                        sizeof(value));
            if (!value) continue;
            std::string name = std::string(kAggregateNames[which]) + "." + route.member;
            if (!route.extension || !has(supported, route.extension)) {
                refusals.push_back(name + "(" + (route.extension ? route.extension : "core-only") + ")");
                continue;
            }
            add_extension(route.extension);
            if (route.stype) {
                VkBaseOutStructure *own = find_in_chain(root, route.stype);
                unsigned char *dst = reinterpret_cast<unsigned char *>(
                    own ? own : extra.get(route.stype, route.struct_size)) + route.struct_offset;
                std::memcpy(dst, &value, sizeof(value));
            }
            routed.push_back(name);
        }
    }
    kept_tail->pNext = extra.link(nullptr);
    if (eds_route && !find_in_chain(root, eds.sType)) {
        VkBaseOutStructure *tail = root;
        while (tail->pNext) tail = tail->pNext;
        tail->pNext = reinterpret_cast<VkBaseOutStructure *>(&eds);
        routed.push_back("vk13.extendedDynamicState(implicit-core)");
    }
    std::string list;
    for (const std::string &entry : routed) list += (list.empty() ? "" : ",") + entry;
    std::string ext_list;
    for (const std::string &entry : added) ext_list += (ext_list.empty() ? "" : ",") + entry;
    dxvk_telemetry_emit("WARN", "DXVK_COMPAT_DEVICE routed=%s added_extensions=%s",
                        list.empty() ? "none" : list.c_str(), ext_list.empty() ? "none" : ext_list.c_str());
    VkResult result;
    if (!refusals.empty()) {
        for (const std::string &entry : refusals)
            dxvk_telemetry_emit("ERR", "DXVK_COMPAT_REFUSAL call=vkCreateDevice feature=%s", entry.c_str());
        std::string all;
        for (const std::string &entry : refusals) all += (all.empty() ? "" : ",") + entry;
        dxvk_telemetry_refusal("compat", "vkCreateDevice", int(VK_ERROR_FEATURE_NOT_PRESENT),
                               ("untranslatable=" + all).c_str());
        result = VK_ERROR_FEATURE_NOT_PRESENT;
    } else {
        VkDeviceCreateInfo patched = *info;
        patched.enabledExtensionCount = uint32_t(extensions.size());
        patched.ppEnabledExtensionNames = extensions.data();
        result = real(physical, &patched, allocator, device);
    }
    for (auto &entry : saved) entry.first->pNext = entry.second;
    return result;
}

const char *compat_add_instance_extension(const VkInstanceCreateInfo *info,
                                          VkInstanceCreateInfo *patched,
                                          const char **storage, uint32_t capacity)
{
    const char *name = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    if (!info || info->enabledExtensionCount + 1 > capacity) return nullptr;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        storage[i] = info->ppEnabledExtensionNames[i];
        if (!std::strcmp(storage[i], name)) return nullptr;
    }
    storage[info->enabledExtensionCount] = name;
    *patched = *info;
    patched->enabledExtensionCount = info->enabledExtensionCount + 1;
    patched->ppEnabledExtensionNames = storage;
    dxvk_telemetry_emit("WARN", "DXVK_COMPAT_INSTANCE added=%s", name);
    return name;
}

#endif
