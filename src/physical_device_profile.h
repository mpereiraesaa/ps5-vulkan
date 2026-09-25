#ifndef PS5VK_PHYSICAL_DEVICE_PROFILE_H
#define PS5VK_PHYSICAL_DEVICE_PROFILE_H

#include "vk_descriptor.h"
#include "graphics_limits.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

/* Values in this profile describe ps5vk's executable frontend contract.  They
 * are intentionally derived from allocator, descriptor and command-encoder
 * bounds; they are not estimates of the PS5's total hardware capability. */
struct ps5vk_physical_profile_info {
    const char *name;
    uint32_t vendor_id;
    uint32_t device_id;
    VkDeviceSize heap_size;
    VkDeviceSize allocation_granularity;
    VkDeviceSize buffer_image_granularity;
    VkBool32 host_coherent;
};

static inline int ps5vk_profile_power_of_two(VkDeviceSize value)
{ return value && !(value & (value - 1)); }

/* Deterministic, public, non-secret compatibility identity for
 * VkPhysicalDeviceProperties::pipelineCacheUUID. Any change to an input below
 * must invalidate previously exported cache data:
 *   - vendor/device identity of this frontend
 *   - GFX1013 target
 *   - driver version reported in the same properties block
 *   - pinned compiler identity and version (PSBC/ACO, see compilation_cache.h)
 *   - cache ABI revision and this UUID format counter
 * The mixing is FNV-1a over two streams; it is an identity, not a security
 * boundary, and it never depends on console state or private material. */
#define PS5VK_PIPELINE_CACHE_UUID_FORMAT 1u
#define PS5VK_GFX_TARGET 1013u
#define PS5VK_DRIVER_VERSION 1u
#define PS5VK_COMPILER_IDENTITY 0x50534243u /* "PSBC" */
#define PS5VK_COMPILER_IDENTITY_VERSION 1u
#define PS5VK_CACHE_ABI_IDENTITY 1u

/* The Vulkan version the physical device reports: the single promotion
 * switch. Raising it claims every mandatory command, feature, limit and
 * behaviour of each newer core version, and tools/check_core_version_contract.py
 * (run by make check) refuses the edit until
 * conformance_inventory/core_version_contract.json shows that whole contract
 * met. The instance version (PS5VK_INSTANCE_API_VERSION) is separate. */
#define PS5VK_DEVICE_API_VERSION VK_API_VERSION_1_0

/* Vulkan 1.0 mandatory floors that both shipped frontends honour.
 *
 * These are the values the specification's "Required Limits" table demands and
 * the pinned CTS checks unconditionally (vktApiFeatureInfo.cpp, the
 * checkAlways rows). They are reported because this frontend does not restrict
 * the corresponding operation below the floor, not because GFX1013 was
 * measured: texture-unit precision and the compiled VS/FS interface are the
 * GPU's and PSBC/ACO's business, so the least a conformant report may claim is
 * the floor itself. Anything this frontend genuinely restricts stays below the
 * floor and is recorded as a blocker in PHYSICAL_DEVICE_REPORTING.md instead. */
#define PS5VK_REQUIRED_SUBTEXEL_BITS 4u
#define PS5VK_REQUIRED_MIPMAP_PRECISION_BITS 4u
/* src/spirv_graphics_interface.c reflects at most LOCATIONS = 32 interface
 * locations of four components, so 128 components are representable. */
#define PS5VK_REQUIRED_INTERFACE_COMPONENTS 64u
#define PS5VK_REQUIRED_SAMPLE_MASK_WORDS 1u
/* Vulkan 1.0 requires at least two discrete normalized queue priorities.  The
 * frontend exposes one serial queue, so priorities cannot compete with one
 * another, but device creation still maps the requested [0,1] value into the
 * required low/high classes instead of reporting an impossible zero. */
#define PS5VK_REQUIRED_QUEUE_PRIORITIES 2u
/* largePoints and wideLines are VK_FALSE, so the only sizes the pipeline
 * accepts are the fixed 1.0 values the mandatory floor requires; the matching
 * granularity limits stay 0, which is what the CTS applies for unsupported
 * features. */
#define PS5VK_REQUIRED_POINT_SIZE 1.0f
#define PS5VK_REQUIRED_LINE_WIDTH 1.0f
/* User-defined clip and cull distances. The two packed position registers after
 * POS0 carry eight float components: clip components first, cull continuing
 * immediately after them, so the combined budget is eight and either feature may
 * use all eight on its own. Vulkan 1.0 requires each of the three limits to be
 * at least eight, and eight is what this frontend can execute: the stage
 * interface refuses a declaration whose combined width exceeds those two
 * registers (src/spirv_graphics_interface.c), and the native export packs clip
 * then cull contiguously into them. */
#define PS5VK_REQUIRED_CLIP_DISTANCES 8u
#define PS5VK_REQUIRED_CULL_DISTANCES 8u
#define PS5VK_REQUIRED_COMBINED_CLIP_CULL_DISTANCES 8u

static inline void ps5vk_profile_mix(uint64_t *h, uint32_t value)
{
    for (unsigned byte = 0; byte < 4; ++byte) {
        *h ^= (uint64_t)((value >> (8 * byte)) & 0xffu);
        *h *= UINT64_C(0x100000001b3);
    }
}

static inline void ps5vk_pipeline_cache_uuid(uint8_t out[VK_UUID_SIZE],
                                             uint32_t vendor_id, uint32_t device_id)
{
    const char tag[] = "ps5vk-gfx1013-pipeline-cache";
    uint64_t a = UINT64_C(0xcbf29ce484222325);
    uint64_t b = UINT64_C(0x9e3779b97f4a7c15);
    for (size_t i = 0; i < sizeof(tag) - 1; ++i) ps5vk_profile_mix(&a, (uint8_t)tag[i]);
    for (size_t i = 0; i < sizeof(tag) - 1; ++i) ps5vk_profile_mix(&b, (uint8_t)tag[sizeof(tag) - 2 - i]);
    ps5vk_profile_mix(&a, vendor_id);   ps5vk_profile_mix(&b, device_id);
    ps5vk_profile_mix(&a, device_id);   ps5vk_profile_mix(&b, vendor_id);
    ps5vk_profile_mix(&a, PS5VK_GFX_TARGET);             ps5vk_profile_mix(&b, PS5VK_GFX_TARGET ^ 0x5a5a5a5au);
    ps5vk_profile_mix(&a, PS5VK_DRIVER_VERSION);         ps5vk_profile_mix(&b, PS5VK_DRIVER_VERSION);
    ps5vk_profile_mix(&a, PS5VK_COMPILER_IDENTITY);      ps5vk_profile_mix(&b, PS5VK_COMPILER_IDENTITY);
    ps5vk_profile_mix(&a, PS5VK_COMPILER_IDENTITY_VERSION); ps5vk_profile_mix(&b, PS5VK_COMPILER_IDENTITY_VERSION);
    ps5vk_profile_mix(&a, PS5VK_CACHE_ABI_IDENTITY);     ps5vk_profile_mix(&b, PS5VK_CACHE_ABI_IDENTITY);
    ps5vk_profile_mix(&a, PS5VK_PIPELINE_CACHE_UUID_FORMAT); ps5vk_profile_mix(&b, PS5VK_PIPELINE_CACHE_UUID_FORMAT);
    for (unsigned i = 0; i < 8; ++i) {
        out[i] = (uint8_t)((a >> (8 * i)) & 0xffu);
        out[8 + i] = (uint8_t)((b >> (8 * i)) & 0xffu);
    }
}

static inline uint32_t ps5vk_profile_u32(VkDeviceSize value)
{ return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value; }

/* The per-resource bound: maxStorageBufferRange, maxMemoryAllocationCount and
 * the image maxResourceSize derive from min(heap, this), never from the heap
 * alone. The heap and maxMemoryAllocationSize may be larger (the graphics
 * profile allows one 1 GiB allocation) without widening what a single
 * descriptor range or image claims. */
#define PS5VK_PROFILE_RESOURCE_LIMIT_BYTES (UINT64_C(256) * 1024 * 1024)
static inline VkDeviceSize ps5vk_profile_resource_limit(VkDeviceSize budget)
{ return budget < PS5VK_PROFILE_RESOURCE_LIMIT_BYTES ? budget : PS5VK_PROFILE_RESOURCE_LIMIT_BYTES; }

static inline void ps5vk_physical_profile_init(
    VkPhysicalDeviceProperties *properties,
    VkPhysicalDeviceMemoryProperties *memory,
    const struct ps5vk_physical_profile_info *info)
{
    memset(properties, 0, sizeof(*properties));
    memset(memory, 0, sizeof(*memory));

    properties->apiVersion = PS5VK_DEVICE_API_VERSION;
    properties->driverVersion = 1;
    properties->vendorID = info->vendor_id;
    properties->deviceID = info->device_id;
    ps5vk_pipeline_cache_uuid(properties->pipelineCacheUUID, info->vendor_id, info->device_id);
    properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    if (info->name) {
        strncpy(properties->deviceName, info->name,
                VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
        properties->deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1] = '\0';
    }

    VkPhysicalDeviceLimits *limits = &properties->limits;
    const VkDeviceSize resource_limit = ps5vk_profile_resource_limit(info->heap_size);
    limits->maxStorageBufferRange = ps5vk_profile_u32(resource_limit);
    limits->maxUniformBufferRange = 64u * 1024u;
    limits->maxTexelBufferElements = 64u * 1024u;
    limits->maxPushConstantsSize = PS5VK_MAX_PUSH_CONSTANT_BYTES;
    /* One indirect draw per command until a platform mask carries
     * PS5VK_FEATURE_MULTI_DRAW_INDIRECT; ps5vk_device_profile_init raises this
     * to the core floor from that mask (vk_internal.h). */
    limits->maxDrawIndirectCount = 1;
    limits->maxMemoryAllocationCount = info->allocation_granularity
        ? ps5vk_profile_u32(resource_limit / info->allocation_granularity) : 0;
    limits->bufferImageGranularity = info->buffer_image_granularity;
    limits->minMemoryMapAlignment = 64;
    limits->minTexelBufferOffsetAlignment = 4;
    limits->minStorageBufferOffsetAlignment = 256;
    limits->minUniformBufferOffsetAlignment = 256;
    limits->nonCoherentAtomSize = 64;
    limits->subTexelPrecisionBits = PS5VK_REQUIRED_SUBTEXEL_BITS;
    limits->mipmapPrecisionBits = PS5VK_REQUIRED_MIPMAP_PRECISION_BITS;
    limits->maxVertexOutputComponents = PS5VK_REQUIRED_INTERFACE_COMPONENTS;
    limits->maxFragmentInputComponents = PS5VK_REQUIRED_INTERFACE_COMPONENTS;
    limits->pointSizeRange[0] = PS5VK_REQUIRED_POINT_SIZE;
    limits->pointSizeRange[1] = PS5VK_REQUIRED_POINT_SIZE;
    limits->lineWidthRange[0] = PS5VK_REQUIRED_LINE_WIDTH;
    limits->lineWidthRange[1] = PS5VK_REQUIRED_LINE_WIDTH;
    limits->maxSampleMaskWords = PS5VK_REQUIRED_SAMPLE_MASK_WORDS;
    limits->discreteQueuePriorities = PS5VK_REQUIRED_QUEUE_PRIORITIES;
    /* Reported unconditionally: the three floors are mandatory limits, and the
     * executable width is the same eight components whether or not the logical
     * device enables a distance feature (the feature decides whether a shader
     * may declare them, not how wide the registers are). */
    limits->maxClipDistances = PS5VK_REQUIRED_CLIP_DISTANCES;
    limits->maxCullDistances = PS5VK_REQUIRED_CULL_DISTANCES;
    limits->maxCombinedClipAndCullDistances = PS5VK_REQUIRED_COMBINED_CLIP_CULL_DISTANCES;
    limits->maxBoundDescriptorSets = PS5VK_MAX_SETS;
    limits->maxPerStageDescriptorStorageBuffers = PS5VK_MAX_DESCRIPTORS;
    limits->maxDescriptorSetStorageBuffers = PS5VK_MAX_DESCRIPTORS;
    limits->maxPerStageDescriptorUniformBuffers = PS5VK_MAX_DESCRIPTORS;
    limits->maxDescriptorSetUniformBuffers = PS5VK_MAX_DESCRIPTORS;
    /* Dynamic UBO/SSBO descriptors share the same compiler table and exact
     * bind-time range validation as their static forms.  Report the Vulkan
     * 1.0 floors conservatively even though the table can hold more. */
    limits->maxDescriptorSetUniformBuffersDynamic = 8;
    limits->maxDescriptorSetStorageBuffersDynamic = 4;
    /* Uniform texel buffers consume the sampled-image accounting class. */
    limits->maxPerStageDescriptorSampledImages = 1;
    limits->maxDescriptorSetSampledImages = 1;
    limits->maxPerStageResources = PS5VK_MAX_DESCRIPTORS;
    /* The public format table has typed UINT/SINT sampled-image execution.
     * Both profiles expose those query paths, conservatively at one sample. */
    limits->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;

    /* dispatch_encode.c enforces each dimension, the product and group count.
     * PSBC configures 64 KiB LDS per GFX1013 workgroup. */
    limits->maxComputeSharedMemorySize = 64u * 1024u;
    limits->maxComputeWorkGroupInvocations = 1024;
    for (unsigned i = 0; i < 3; ++i) {
        limits->maxComputeWorkGroupCount[i] = 65535;
        limits->maxComputeWorkGroupSize[i] = 1024;
    }

    memory->memoryHeapCount = 1;
    memory->memoryTypeCount = 1;
    memory->memoryHeaps[0].size = info->heap_size;
    memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    memory->memoryTypes[0].heapIndex = 0;
    memory->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        (info->host_coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0);
}

/* The optional second memory type: the same heap and backing as type 0, plus
 * HOST_COHERENT. Coherence is maintained by the driver, not by a different
 * mapping: every mapped allocation of this type is written back from the CPU
 * caches before each GPU submission launches and invalidated after each
 * observed completion, and on map/unmap (src/vk_memory.c); each GPU
 * submission already starts with a full GPU cache invalidate and ends with an
 * L2 writeback. It is appended after type 0, so a consumer that takes the
 * first host-visible type keeps type 0. */
static inline void ps5vk_profile_add_coherent_type(VkPhysicalDeviceMemoryProperties *memory)
{
    if (memory->memoryTypeCount != 1) return;
    memory->memoryTypes[1] = memory->memoryTypes[0];
    memory->memoryTypes[1].propertyFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memory->memoryTypeCount = 2;
}

static inline int ps5vk_physical_profile_valid(
    const VkPhysicalDeviceProperties *properties,
    const VkPhysicalDeviceMemoryProperties *memory,
    VkDeviceSize max_allocation,
    VkQueueFlags queue_flags,
    int has_format_properties,
    int has_image_properties)
{
    const VkPhysicalDeviceLimits *limits = &properties->limits;
    if (properties->apiVersion != PS5VK_DEVICE_API_VERSION ||
        !properties->deviceName[0] ||
        properties->deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1] != '\0' ||
        !max_allocation || !queue_flags ||
        (queue_flags & ~(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                         VK_QUEUE_TRANSFER_BIT)) ||
        !limits->maxStorageBufferRange ||
        limits->maxStorageBufferRange > max_allocation ||
        !limits->maxUniformBufferRange || !limits->maxTexelBufferElements ||
        !limits->maxPushConstantsSize || !limits->maxMemoryAllocationCount ||
        !ps5vk_profile_power_of_two(limits->bufferImageGranularity) ||
        !ps5vk_profile_power_of_two(limits->minMemoryMapAlignment) ||
        !ps5vk_profile_power_of_two(limits->minTexelBufferOffsetAlignment) ||
        !ps5vk_profile_power_of_two(limits->minUniformBufferOffsetAlignment) ||
        !ps5vk_profile_power_of_two(limits->minStorageBufferOffsetAlignment) ||
        !ps5vk_profile_power_of_two(limits->nonCoherentAtomSize) ||
        limits->subTexelPrecisionBits < PS5VK_REQUIRED_SUBTEXEL_BITS ||
        limits->mipmapPrecisionBits < PS5VK_REQUIRED_MIPMAP_PRECISION_BITS ||
        limits->maxVertexOutputComponents < PS5VK_REQUIRED_INTERFACE_COMPONENTS ||
        limits->maxFragmentInputComponents < PS5VK_REQUIRED_INTERFACE_COMPONENTS ||
        limits->maxClipDistances < PS5VK_REQUIRED_CLIP_DISTANCES ||
        limits->maxCullDistances < PS5VK_REQUIRED_CULL_DISTANCES ||
        limits->maxCombinedClipAndCullDistances <
            PS5VK_REQUIRED_COMBINED_CLIP_CULL_DISTANCES ||
        limits->pointSizeRange[0] < PS5VK_REQUIRED_POINT_SIZE ||
        limits->pointSizeRange[1] < PS5VK_REQUIRED_POINT_SIZE ||
        limits->lineWidthRange[0] < PS5VK_REQUIRED_LINE_WIDTH ||
        !(limits->sampledImageIntegerSampleCounts & VK_SAMPLE_COUNT_1_BIT) ||
        limits->lineWidthRange[1] < PS5VK_REQUIRED_LINE_WIDTH ||
        !limits->maxSampleMaskWords ||
        limits->discreteQueuePriorities < PS5VK_REQUIRED_QUEUE_PRIORITIES ||
        limits->maxBoundDescriptorSets != PS5VK_MAX_SETS ||
        limits->maxPerStageResources != PS5VK_MAX_DESCRIPTORS ||
        !limits->maxComputeSharedMemorySize ||
        !limits->maxComputeWorkGroupInvocations)
        return 0;
    for (unsigned i = 0; i < 3; ++i)
        if (!limits->maxComputeWorkGroupCount[i] ||
            !limits->maxComputeWorkGroupSize[i]) return 0;

    /* A second type is only the coherent variant of type 0 on the same heap. */
    if (memory->memoryTypeCount == 2 &&
        (memory->memoryTypes[1].heapIndex != 0 ||
         (memory->memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
         memory->memoryTypes[1].propertyFlags !=
            (memory->memoryTypes[0].propertyFlags | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
        return 0;
    if (memory->memoryTypeCount < 1 || memory->memoryTypeCount > 2 ||
        memory->memoryHeapCount != 1 ||
        memory->memoryTypes[0].heapIndex != 0 ||
        !(memory->memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
        !(memory->memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        (memory->memoryTypes[0].propertyFlags &
            ~(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ||
        !(memory->memoryHeaps[0].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ||
        (memory->memoryHeaps[0].flags & ~VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ||
        memory->memoryHeaps[0].size < max_allocation)
        return 0;

    if (!!has_format_properties != !!has_image_properties) return 0;
    if (queue_flags & VK_QUEUE_GRAPHICS_BIT) {
        /* The four sampled-descriptor limits must stay between the qualified
         * minima the witnesses reach and the capacity the descriptor table
         * layout implements. A graphics profile that drops below the floor, or
         * claims more records than one table can carry, fails closed here
         * instead of reaching the report. The compute-only build applies no
         * graphics limits and keeps reporting them as documented blockers. */
        if (limits->maxPerStageDescriptorSamplers < PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS ||
            limits->maxPerStageDescriptorSamplers > PS5VK_MAX_DESCRIPTORS ||
            limits->maxPerStageDescriptorSampledImages < PS5VK_QUALIFIED_STAGE_SAMPLED_DESCRIPTORS ||
            limits->maxPerStageDescriptorSampledImages > PS5VK_MAX_DESCRIPTORS ||
            limits->maxDescriptorSetSamplers < PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS ||
            limits->maxDescriptorSetSamplers > PS5VK_MAX_DESCRIPTORS ||
            limits->maxDescriptorSetSampledImages < PS5VK_QUALIFIED_SET_SAMPLED_DESCRIPTORS ||
            limits->maxDescriptorSetSampledImages > PS5VK_MAX_DESCRIPTORS)
            return 0;
        if (!has_format_properties || !limits->maxImageDimension2D ||
            !limits->maxFramebufferWidth || !limits->maxFramebufferHeight)
            return 0;
    } else if (has_format_properties || has_image_properties) {
        return 0;
    }
    return 1;
}

#endif
