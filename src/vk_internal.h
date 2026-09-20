#ifndef PS5VK_VK_INTERNAL_H
#define PS5VK_VK_INTERNAL_H

#include <vulkan/vulkan_core.h>
#include <stddef.h>
#include "graphics_limits.h"

/* Legacy private diagnostic override, OFF unless a probe asks for it.
 * It changes no public query. Normal builds use the platform capability and
 * the feature enabled on the device, rather than this measurement override. */
#ifndef PS5VK_MULTIVIEW_DIAGNOSTIC
#define PS5VK_MULTIVIEW_DIAGNOSTIC 0
#endif
/* Private diagnostic gate for the optional-stage witnesses (geometry,
 * tessellation and the clip/cull distances they export). The shipping path
 * requires the logical device to have enabled the feature; only a build that
 * exists to measure the hardware skips that negotiation, exactly as the
 * multiview witness build does. */
#ifndef PS5VK_OPTIONAL_STAGE_DIAGNOSTIC
#define PS5VK_OPTIONAL_STAGE_DIAGNOSTIC 0
#endif
/* The widest mask the diagnostic build validates against, and therefore the most
 * layers the framebuffer rule below has to be able to serve. */
enum { PS5VK_MULTIVIEW_DIAGNOSTIC_VIEWS = 6 };

/* Internal backend seam, not a Vulkan extension or public creation API.
 * Host tests supply instrumented storage; native code must supply direct memory.
 * No allocator is implicitly selected and no GPU execution is emulated here. */
struct ps5vk_memory_backend {
    void *context;
    VkResult (*allocate)(void *, VkDeviceSize, void **, void **);
    void (*release)(void *, void *);
    VkResult (*flush)(void *, void *, VkDeviceSize, VkDeviceSize);
    VkResult (*invalidate)(void *, void *, VkDeviceSize, VkDeviceSize);
};

struct ps5vk_memory_backend ps5vk_native_memory_backend(void);
struct ps5vk_memory_backend ps5vk_native_graphics_memory_backend(void);
VkResult ps5vk_native_image_requirements(VkDevice, const VkImageCreateInfo *, VkMemoryRequirements *);
/* Storage of one array layer of a color/depth attachment surface, and of a
 * whole layered attachment: stride == the per-layer footprint slice A measured,
 * bytes == stride * layers with an explicit overflow check. layers == 1
 * reproduces the single-layer requirements exactly. */
VkResult ps5vk_native_layered_storage(VkFormat, uint32_t width, uint32_t height,
    uint64_t layers, VkDeviceSize *stride, VkDeviceSize *alignment, VkDeviceSize *bytes);
struct ps5vk_native_memory_budget { uint64_t limit, used; };
void ps5vk_native_queue_configure(VkDevice device);
struct ps5vk_compiled_program;
struct ps5vk_graphics_key;
enum ps5vk_feature_bits {
    PS5VK_FEATURE_STORAGE_BUFFER_8BIT = 1u << 0,
    PS5VK_FEATURE_STORAGE_BUFFER_16BIT = 1u << 1,
    /* Vulkan 1.0 core robustness.  Buffer SRDs carry the exact descriptor
     * byte extent and select GFX10 raw OOB checking; vertex fetch descriptors
     * are likewise bounded by the bound VkBuffer span. */
    PS5VK_FEATURE_ROBUST_BUFFER_ACCESS = 1u << 2,
    /* VK_KHR_shader_draw_parameters on the Vulkan 1.0 profile. The supported
     * contract is the one this tranche witnessed: BaseVertex, BaseInstance and
     * DrawIndex for direct draws and for a single indirect draw, with DrawIndex
     * delivered as zero because multiDrawIndirect stays false. Requesting more
     * than one draw per command is still refused. */
    PS5VK_FEATURE_SHADER_DRAW_PARAMETERS = 1u << 3,
    /* VK_KHR_multiview on the measured graphics/runtime-compiler path.
     * Public feature/property queries and device enablement use this bit;
     * platforms that cannot execute multiview leave it unset. */
    PS5VK_FEATURE_MULTIVIEW = 1u << 4,
    /* Vulkan 1.0 core indirect and indexed draw features (DXVK262-T03). Each
     * bit is set by a platform only when the executable path behind it exists
     * and was measured; the logical device carries the bits the application
     * enabled, and the indirect frontend consults THOSE, not the physical
     * mask, so an application that did not enable a feature keeps the
     * fail-closed rules of a device without it. */
    /* VkDraw*IndirectCommand::firstInstance may be non-zero. */
    PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE = 1u << 5,
    /* vkCmdDraw*Indirect drawCount may exceed one; DrawIndex is the command
     * index and maxDrawIndirectCount is the core floor of 65535. */
    PS5VK_FEATURE_MULTI_DRAW_INDIRECT = 1u << 6,
    /* The full 32-bit range of VK_INDEX_TYPE_UINT32 indices. */
    PS5VK_FEATURE_FULL_DRAW_INDEX_UINT32 = 1u << 7,
    /* Optional graphics stages (DXVK262-T04), which start after the indirect
     * draw tranche. Each bit means "this device can and does deliver the
     * capability", and the shipping gates refuse a shader that uses a feature
     * whose bit the logical device did not enable. */
    PS5VK_FEATURE_SHADER_CLIP_DISTANCE = 1u << 8,
    PS5VK_FEATURE_SHADER_CULL_DISTANCE = 1u << 9,
    PS5VK_FEATURE_GEOMETRY_SHADER = 1u << 10,
    PS5VK_FEATURE_TESSELLATION_SHADER = 1u << 11,
    /* Rasterization and viewport state (DXVK262-T05), which start after the
     * optional-stage tranche so the two integrations union without renumbering.
     * Each bit is set by a platform only when the native path behind it
     * programs the state and was measured; the logical device carries the bits
     * the application enabled and the pipeline/command frontends consult
     * THOSE. No shipping profile sets any of them yet, and the private
     * diagnostic guard in the native platform is what lets the T05 witness
     * negotiate them before anything is advertised. */
    /* depthBiasClamp: a non-zero clamp in static or dynamic depth bias. */
    PS5VK_FEATURE_DEPTH_BIAS_CLAMP = 1u << 12,
    /* depthClamp: depthClampEnable replaces near/far clipping by clamping. */
    PS5VK_FEATURE_DEPTH_CLAMP = 1u << 13,
    /* fillModeNonSolid: VK_POLYGON_MODE_LINE and VK_POLYGON_MODE_POINT. */
    PS5VK_FEATURE_FILL_MODE_NON_SOLID = 1u << 14,
    /* multiViewport: viewport/scissor arrays up to maxViewports. */
    PS5VK_FEATURE_MULTI_VIEWPORT = 1u << 15,
    /* Fragment output, blending and multisampling (DXVK262-T06). These bits
     * describe four independent contracts. A platform advertises one only
     * after its complete native path has been measured; until then the core
     * feature table reports false and device creation refuses the request.
     * Keeping them separate is important: supporting ordinary per-target
     * blending does not imply a second fragment output, and accepting a
     * multisample create-info does not prove per-sample shader execution. */
    PS5VK_FEATURE_INDEPENDENT_BLEND = 1u << 16,
    PS5VK_FEATURE_DUAL_SRC_BLEND = 1u << 17,
    PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS = 1u << 18,
    PS5VK_FEATURE_SAMPLE_RATE_SHADING = 1u << 19,
};

/* The maxDrawIndirectCount a platform mask commits to: the pinned core table
 * requires 2^16-1 once multiDrawIndirect is supported and exactly 1 otherwise.
 * One helper decides it so the physical limit, the recording bound and the
 * queue-head re-validation cannot disagree. */
enum { PS5VK_MULTI_DRAW_INDIRECT_COUNT = 65535 };
static inline uint32_t ps5vk_platform_max_draw_indirect_count(uint32_t supported_features)
{
    return (supported_features & PS5VK_FEATURE_MULTI_DRAW_INDIRECT) ?
        (uint32_t)PS5VK_MULTI_DRAW_INDIRECT_COUNT : 1u;
}

/* The maxViewports a platform mask commits to: the pinned core table requires
 * 16 once multiViewport is supported and allows exactly 1 otherwise. One helper
 * decides it so the physical limit, the pipeline's array capacity and the
 * setters' range checks cannot disagree (DXVK262-T05). */
static inline uint32_t ps5vk_platform_max_viewports(uint32_t supported_features)
{
    return (supported_features & PS5VK_FEATURE_MULTI_VIEWPORT) ?
        (uint32_t)PS5VK_MULTI_VIEWPORT_COUNT : 1u;
}

/* The measured multiview floors: six views rendered into six ordered array
 * layers, and one instance at firstInstance 0x07ffffff (2^27-1). Both are
 * measured floors, also used as the conservative public KHR property values. */
enum {
    PS5VK_MULTIVIEW_VIEW_COUNT_FLOOR = 6,
    PS5VK_MULTIVIEW_INSTANCE_INDEX_FLOOR = 134217727,
};

/* Internal capability gate for the multiview work: true only when the platform
 * mask carries the bit, so a build or platform without it reports the internal
 * support as false rather than inheriting an assumption from the profile. */
static inline int ps5vk_platform_multiview_supported(uint32_t supported_features)
{
    return !!(supported_features & PS5VK_FEATURE_MULTIVIEW);
}
struct ps5vk_compiler {
    void *context;
    VkResult (*resolve)(void *, const uint32_t *, size_t, const char *,
                        const struct ps5vk_compiled_program **);
    VkResult (*compile)(void *, const uint32_t *, size_t, const char *,
                        VkPipelineLayout, const VkSpecializationInfo *, uint32_t,
                        struct ps5vk_compiled_program *, uint32_t **);
};
struct ps5vk_progress {
    void *context;
    VkResult (*poll)(VkDevice);
    uint64_t (*clock_ns)(void *);
    void (*pause)(void *, uint64_t);
};
struct ps5vk_submission;
struct ps5vk_queue_backend {
    /* prepare must not submit. On failure it must retain no returned job. */
    VkResult (*prepare)(VkDevice, const struct ps5vk_submission *, void **);
    VkResult (*launch)(VkDevice, void *);
    /* completed=0 means still pending; nonzero must be the exact submitted
     * serial, observed after GPU cache writeback/visibility, not just a label. */
    VkResult (*poll)(VkDevice, void *, uint64_t *completed);
    void (*release)(VkDevice, void *);
};

/* Link-selected platform implementation. Production must query/configure its
 * native backend; test binaries provide explicit mock implementations. */
struct ps5vk_platform {
    VkQueueFlags queue_flags;
    void (*format_properties)(VkFormat, VkFormatProperties *);
    VkResult (*image_properties)(VkFormat,VkImageType,VkImageTiling,
        VkImageUsageFlags,VkImageCreateFlags,VkDeviceSize,VkImageFormatProperties *);
    struct ps5vk_queue_backend queue_backend;
    struct ps5vk_progress progress;
    struct ps5vk_compiler compiler;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkDeviceSize max_allocation;
    /* Platform opt-in only. A frontend symbol or compiler path is not enough
     * to advertise a Vulkan feature without a native backend contract. */
    uint32_t supported_features;
    void *context;
    VkResult (*open)(void *, struct ps5vk_memory_backend *);
    void (*close)(struct ps5vk_memory_backend *);
    void (*configure)(VkDevice);
};
VkResult ps5vk_platform_query(struct ps5vk_platform *platform);

struct VkPhysicalDevice_T {
    VkInstance instance;
    struct ps5vk_platform platform;
};
struct VkInstance_T {
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    struct VkPhysicalDevice_T physical;
    VkBool32 features2_extension_enabled;
    unsigned devices, lifetime_errors;
};
struct VkQueue_T {
    VkDevice device;
    uint64_t next_serial, completed_serial;
    /* 0=low, 1=high. With the single exposed queue this cannot affect
     * inter-queue scheduling, but preserves the normalized creation contract. */
    uint32_t priority_class;
};

struct VkDevice_T {
    VkPhysicalDevice physical;
    struct VkQueue_T queue;
    struct ps5vk_memory_backend memory;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkDeviceSize buffer_alignment;
    VkDeviceSize uniform_buffer_alignment;
    VkDeviceSize noncoherent_atom;
    VkDeviceSize max_allocation;
    uint32_t enabled_features;
    struct VkDeviceMemory_T *memories;
    struct VkBuffer_T *buffers;
    struct VkBufferView_T *buffer_views;
    unsigned lifetime_errors;
    unsigned descriptor_objects;
    unsigned pipeline_objects;
    unsigned graphics_objects;
    unsigned sampler_objects;
    VkBool32 graphics_enabled;
    /* Set only by a backend implementing graphics prepare/execute/visibility.
     * Object creation or shader linking alone does not enable submission. */
    VkBool32 graphics_submit_enabled;
    const struct ps5vk_graphics_library *graphics_library;
    /* Runtime result lease: create copies everything it needs into owned GPU
     * state before release. Acquire may compile or return a cached result.
     * Both callbacks are required; no implicit offline fallback on failure. */
    void *graphics_compiler_context;
    VkResult (*graphics_acquire)(void *,const struct ps5vk_graphics_key *,const void **);
    void (*graphics_compiled_release)(void *,const void *);
    /* The backend receives the GFX1013 primitive type the pipeline's topology
     * maps to, so the AGC link step and the compiled shader cannot disagree. */
    VkResult (*graphics_create)(VkDevice, const void *program_data,
        uint32_t primitive_type, void **owned_state);
    void (*graphics_release)(VkDevice, void *owned_state);
    /* Optional immutable executable-usage summary after native creation.
     * Absence means unknown: all nonempty layout sets remain required. */
    VkResult (*graphics_used_sets)(VkDevice, const void *owned_state, uint32_t *mask);
    VkResult (*image_requirements)(VkDevice, const VkImageCreateInfo *, VkMemoryRequirements *);
    struct VkImage_T *images;
    struct ps5vk_compiler compiler;
    struct ps5vk_compilation_cache *pipeline_cache;
    VkBool32 runtime_compiler_enabled;
    struct VkCommandPool_T *command_pools;
    VkBool32 (*invalidate)(VkDevice, VkObjectType, const void *);
    struct VkFence_T *fences;
    struct VkPipelineCache_T *pipeline_caches;
    struct VkQueryPool_T *query_pools;
    struct VkSemaphore_T *semaphores;
    struct VkEvent_T *events;
    struct ps5vk_progress progress;
    struct ps5vk_queue_backend submit_backend;
    struct ps5vk_queue_backend compute_backend, graphics_backend;
    struct ps5vk_submission *submission;
    VkBool32 lost;
};

void ps5vk_device_enable_runtime_compiler(VkDevice device);

void *ps5vk_object_alloc(const VkAllocationCallbacks *fallback,
    const VkAllocationCallbacks *given, size_t size, VkSystemAllocationScope scope,
    VkAllocationCallbacks *saved, VkBool32 *custom);
void ps5vk_object_free(void *object, const VkAllocationCallbacks *saved, VkBool32 custom);

/* Resolve descriptor byte ranges without discarding buffer binding offsets.
 * Used by the descriptor/compiler/backend bridge, never a Vulkan device address. */
/* Flush the exact bound allocation range carrying a driver-originated image write. */
VkResult ps5vk_image_flush_range(VkDevice device, VkImage image, VkDeviceSize offset,
                                 VkDeviceSize size);
/* Invalidate the exact bound allocation range before the CPU reads bytes the
 * GPU produced, the read half of the same non-coherent memory contract. */
VkResult ps5vk_image_invalidate_range(VkDevice device, VkImage image, VkDeviceSize offset,
                                      VkDeviceSize size);
VkResult ps5vk_buffer_span(VkDevice device, VkBuffer buffer, VkDeviceSize offset,
                          VkDeviceSize range, void **address, VkDeviceSize *size);
VkResult ps5vk_buffer_cache(VkDevice device, VkBuffer buffer,
                            VkDeviceSize offset, VkDeviceSize range,
                            VkBool32 invalidate);

VkBool32 ps5vk_buffer_usage(VkDevice,VkBuffer,VkBufferUsageFlags);
#endif
