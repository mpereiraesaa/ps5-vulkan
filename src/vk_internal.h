#ifndef PS5VK_VK_INTERNAL_H
#define PS5VK_VK_INTERNAL_H

#include <vulkan/vulkan_core.h>
#include <stddef.h>
#include <pthread.h>
#include "graphics_limits.h"
#include "sample_rate_contract.h"

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
/* Resolve a direct-memory allocation's GPU virtual address. The host default
 * refuses this; native memory provides the address used by GPU descriptors. */
VkResult ps5vk_memory_backend_device_address(void *backing, VkDeviceAddress *out);
/* HOST_COHERENT maintenance at queue boundaries: CPU writeback of every mapped
 * coherent allocation before a GPU submission launches, CPU invalidate after
 * its completion is observed. Both are no-ops without such an allocation. */
VkResult ps5vk_coherent_host_writeback(VkDevice d);
VkResult ps5vk_coherent_host_invalidate(VkDevice d);
VkResult ps5vk_native_image_requirements(VkDevice, const VkImageCreateInfo *, VkMemoryRequirements *);
/* Storage of one array layer of a color/depth attachment surface, and of a
 * whole layered attachment: stride == the per-layer footprint slice A measured,
 * bytes == stride * layers with an explicit overflow check. layers == 1
 * reproduces the single-layer requirements exactly. */
VkResult ps5vk_native_layered_storage(VkFormat, uint32_t width, uint32_t height,
    uint64_t layers, VkDeviceSize *stride, VkDeviceSize *alignment, VkDeviceSize *bytes);
/* The same per-layer footprint with the surface's own sample count: a
 * multisampled colour surface stores one sample plane per sample, so the
 * layer's bytes scale with the count before the same alignment (DXVK262-T06,
 * native/image_ps5.c). 1x reproduces ps5vk_native_layered_storage exactly. */
VkResult ps5vk_native_layered_storage_samples(VkFormat, uint32_t width, uint32_t height,
    uint64_t layers, VkSampleCountFlagBits samples, VkDeviceSize *stride,
    VkDeviceSize *alignment, VkDeviceSize *bytes);
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
    /* Vulkan 1.0 extension features (DXVK262-T08). Keep physical addresses
     * and both memory-model promises independent: device scope requires the
     * base Vulkan memory model, while BDA does not imply either one. */
    PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS = 1u << 20,
    PS5VK_FEATURE_VULKAN_MEMORY_MODEL = 1u << 21,
    PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE = 1u << 22,
    /* T07 features use a separate range so their enabled gates coexist with
     * the T08 Vulkan 1.0 extension features above. */
    PS5VK_FEATURE_IMAGE_CUBE_ARRAY = 1u << 25,
    PS5VK_FEATURE_SHADER_IMAGE_GATHER_EXTENDED = 1u << 26,
    PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE = 1u << 27,
    /* Keep this layout gate independent of the other feature bits. */
    PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT = 1u << 23,
    PS5VK_FEATURE_TEXTURE_COMPRESSION_BC = 1u << 24,
    /* Core Vulkan 1.0 narrow shader arithmetic; diagnostic only. */
    PS5VK_FEATURE_SHADER_INT16 = 1u << 28,
    /* Internal compute Broadcast route. The shipping platform leaves this
     * unset; a private measurement build may exercise the runtime path. It
     * is not a public Vulkan feature or subgroup-properties promise. */
    PS5VK_FEATURE_SUBGROUP_BROADCAST_COMPUTE = 1u << 29,
    /* Independent private compute IAdd route; never a public ARITHMETIC
     * operation or extended-types feature promise. */
    PS5VK_FEATURE_SUBGROUP_IADD_COMPUTE = 1u << 30,
    /* Internal compute compiler probe only. Never maps to VkPhysicalDeviceFeatures. */
    PS5VK_FEATURE_SHADER_INT8_COMPUTE = 1u << 31,
};

/* The original 32-bit feature mask is full once the T08 shader gates land.
 * Keep the independent T09 extension capabilities in a separate mask. */
enum ps5vk_t09_feature_bits {
    PS5VK_T09_FEATURE_HOST_QUERY_RESET = 1u << 0,
    PS5VK_T09_FEATURE_IMAGELESS_FRAMEBUFFER = 1u << 1,
    PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE = 1u << 2,
    PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE = 1u << 3,
    PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS = 1u << 4,

    /* VK_KHR_maintenance2 (no feature structure): image view usage, input
     * attachment aspects, point clipping, tessellation domain origin and the
     * mixed depth/stencil layouts. */
    PS5VK_T09_FEATURE_MAINTENANCE2 = 1u << 5,
    /* VK_KHR_create_renderpass2 (no feature structure). Enumerated only with
     * its registry dependencies, VK_KHR_multiview and VK_KHR_maintenance2. */
    PS5VK_T09_FEATURE_CREATE_RENDERPASS2 = 1u << 6,
    /* DXVK262-T11 pixel removal on the Vulkan 1.0 profile.
     * VK_EXT_shader_demote_to_helper_invocation: OpDemoteToHelperInvocation
     * keeps the invocation running as a helper for its quad's derivatives.
     * VK_KHR_shader_terminate_invocation: OpTerminateInvocation. A platform
     * sets them only once native pixel witnesses measured the behaviour. */
    PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION = 1u << 7,
    PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION = 1u << 8,
    /* Memory-requirement and binding routes DXVK calls right after device
     * creation. Each is a Vulkan 1.0 KHR extension with no feature structure;
     * the platform reports none of them until the native run confirms them. */
    /* VK_KHR_get_memory_requirements2: vkGet{Buffer,Image,ImageSparse}
     * MemoryRequirements2KHR. */
    PS5VK_T09_FEATURE_GET_MEMORY_REQUIREMENTS2 = 1u << 9,
    /* VK_KHR_dedicated_allocation (requires get_memory_requirements2):
     * VkMemoryDedicatedRequirements and VkMemoryDedicatedAllocateInfo. */
    PS5VK_T09_FEATURE_DEDICATED_ALLOCATION = 1u << 10,
    /* VK_KHR_bind_memory2: vkBind{Buffer,Image}Memory2KHR. */
    PS5VK_T09_FEATURE_BIND_MEMORY2 = 1u << 11,
    /* VK_KHR_maintenance4. The pinned registry makes it depend on Vulkan 1.1
     * with no extension alternative, so it stays unavailable while the
     * device reports Vulkan 1.0, whatever the platform reports. */
    PS5VK_T09_FEATURE_MAINTENANCE4 = 1u << 12,
    /* VK_EXT_robustness2 (DXVK262-T13), one bit per implemented feature.
     * robustBufferAccess2: uniform/storage buffer records bound the
     * descriptor range rounded up to PS5VK_ROBUST_BUFFER_ACCESS_SIZE_ALIGNMENT.
     * nullDescriptor: VK_NULL_HANDLE buffers, buffer views and image views are
     * written as zeroed hardware records. robustImageAccess2 is not
     * implemented. No platform reports either bit until the native witness
     * measured it. Bits 20-21 are the range coordinated for this tranche. */
    PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2 = 1u << 20,
    PS5VK_T09_FEATURE_NULL_DESCRIPTOR = 1u << 21,
    /* VK_KHR_descriptor_update_template (no feature structure): a host-side
     * recording of descriptor writes. Enumerated only when the platform sets
     * it, which the shipping platform does not yet. */
    PS5VK_T09_FEATURE_DESCRIPTOR_UPDATE_TEMPLATE = 1u << 15,

    /* DXVK first-draw recording routes (T10 minus synchronization2). Bits
     * 16 and up so concurrent T09-T13 slices can take 7..15 without a
     * collision. Each is an unadvertised platform capability until a native
     * witness proves it; the shipping platform leaves it clear. */
    /* VK_EXT_extended_dynamic_state: the per-draw fixed-function snapshot
     * (viewport/scissor with count, cull mode, front face, depth and stencil
     * test state). Dynamic primitive topology and vertex binding stride are
     * not executed yet, so pipelines declaring them stay refused. */
    PS5VK_T09_FEATURE_EXTENDED_DYNAMIC_STATE = 1u << 16,
    /* VK_KHR_copy_commands2 (no feature structure): the six version-2
     * transfer commands, converted to the version-1 ones. */
    PS5VK_T09_FEATURE_COPY_COMMANDS2 = 1u << 17,
    /* VK_KHR_depth_stencil_resolve (no feature structure), the registry
     * dependency of dynamic rendering on Vulkan 1.0. Every depth attachment is
     * single-sample on this device, so no depth/stencil resolve can be
     * requested; the extension reports the mandatory SAMPLE_ZERO mode and
     * accepts only the structure that names no resolve attachment. */
    PS5VK_T09_FEATURE_DEPTH_STENCIL_RESOLVE = 1u << 18,
    /* VK_KHR_dynamic_rendering: a render pass instance begun from a
     * VkRenderingInfo executes as the equivalent single-subpass render pass. */
    PS5VK_T09_FEATURE_DYNAMIC_RENDERING = 1u << 19,
    /* VK_KHR_maintenance1 (no feature structure): negative viewport heights,
     * vkTrimCommandPoolKHR and the transfer format-feature bits.
     * VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT is refused by every image
     * format query, which the extension permits. */
    PS5VK_T09_FEATURE_MAINTENANCE1 = 1u << 24,
    /* VK_KHR_format_feature_flags2 (no feature structure): VkFormatProperties3
     * answered from the same capability table as VkFormatProperties. */
    PS5VK_T09_FEATURE_FORMAT_FEATURE_FLAGS2 = 1u << 13,
    /* VK_KHR_image_format_list (no feature structure) together with the
     * VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT reinterpretations it lists: only the
     * pairs src/texture_format.c implements (RGBA8 UNORM <-> SRGB). */
    PS5VK_T09_FEATURE_IMAGE_FORMAT_LIST = 1u << 14,
    /* DXVK262 synchronization2 on the Vulkan 1.0 profile:
     * VK_KHR_synchronization2 and its synchronization2 feature. Its commands
     * convert onto the Vulkan 1.0 barrier, event, timestamp and submit
     * paths (src/vk_sync2.c). A platform sets it only once a native witness
     * measured the converted route. */
    PS5VK_T09_FEATURE_SYNCHRONIZATION2 = 1u << 27,
    /* Internal compute subgroup BASIC route (Elect, subgroup barriers and the
     * subgroup built-ins) for a private measurement build. Not a subgroup
     * properties promise: VkPhysicalDeviceSubgroupProperties stays zero. */
    PS5VK_T09_FEATURE_SUBGROUP_BASIC_COMPUTE = 1u << 28,
    /* A second, HOST_COHERENT memory type whose coherence the driver keeps at
     * map/unmap and submission boundaries (src/physical_device_profile.h).
     * Only the diagnostic witness build sets it until native proof. */
    PS5VK_T09_FEATURE_HOST_COHERENT_MEMORY = 1u << 26,
    /* VK_EXT_transform_feedback (DXVK262-T14): transformFeedback and
     * geometryStreams, captured by the geometry stage through the compiler's
     * no-GDS global streamout. One bit; the shipping platform sets it on the
     * public-SDK capture witness's evidence (stream order, resume, overflow,
     * streams, instancing, DrawIndirectByteCount and stream queries). */
    PS5VK_T09_FEATURE_TRANSFORM_FEEDBACK = 1u << 25,
};

/* robust{Storage,Uniform}BufferAccessSizeAlignment. GFX10 raw buffer records
 * (stride 0, NUM_RECORDS in bytes) bounds-check each dword of a VMEM or SMEM
 * access; with NUM_RECORDS a multiple of four no naturally aligned access can
 * straddle the bound, which is the granularity RADV reports for the same
 * hardware. The native witness measures it before any platform reports it. */
enum { PS5VK_ROBUST_BUFFER_ACCESS_SIZE_ALIGNMENT = 4 };

/* maxTimelineSemaphoreValueDifference, derived from the payload algorithm
 * rather than copied from a profile floor: every payload, wait value and
 * signal value is a full uint64_t, and the frontend only ever orders them with
 * full-width comparisons (value >= wait, signal > current, max() on
 * retirement). No difference, modular window or narrower hardware label is
 * computed from them, so any two representable values stay correctly ordered
 * and the largest representable difference is supported. */
#define PS5VK_TIMELINE_MAX_VALUE_DIFFERENCE UINT64_MAX


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

/* The sample counts a platform commits to: the envelope in
 * src/sample_rate_contract.h once the platform carries
 * PS5VK_FEATURE_SAMPLE_RATE_SHADING, and the single-sample baseline otherwise.
 * One helper decides it so the reported framebuffer sample limits, the render
 * pass and framebuffer object model, the graphics pipeline's multisample state
 * and the native attachment plan cannot disagree (DXVK262-T06). A device that
 * never measured a multisample path therefore keeps every one of them at 1x. */
static inline VkSampleCountFlags ps5vk_platform_sample_counts(
    uint32_t supported_features)
{
    return (supported_features & PS5VK_FEATURE_SAMPLE_RATE_SHADING) ?
        ps5vk_sample_count_mask() : VK_SAMPLE_COUNT_1_BIT;
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
    uint32_t supported_features_t09;
    /* DIAGNOSTIC ONLY, never set by a shipping build: open the
     * VK_KHR_maintenance4 route although the device reports Vulkan 1.0, so a
     * diagnostic DXVK run can be measured past it. The registry requires
     * Vulkan 1.1; with this set the route is knowingly non-conformant. */
    VkBool32 maintenance4_diagnostic_on_vulkan_1_0;
    void *context;
    VkResult (*open)(void *, struct ps5vk_memory_backend *);
    void (*close)(struct ps5vk_memory_backend *);
    void (*configure)(VkDevice);
};
VkResult ps5vk_platform_query(struct ps5vk_platform *platform);

/* Vulkan 1.1 instance: any requested apiVersion is accepted, and the only
 * instance-level additions (vkEnumerateInstanceVersion and the core
 * vkEnumeratePhysicalDeviceGroups) are implemented. It does not raise the
 * physical device, which still reports Vulkan 1.0. */
#define PS5VK_INSTANCE_API_VERSION VK_API_VERSION_1_3
struct VkPhysicalDevice_T {
    VkInstance instance;
    struct ps5vk_platform platform;
};
struct VkSurfaceKHR_T {
    VkInstance instance;
    VkExtent2D extent;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    struct VkSurfaceKHR_T *next;
    unsigned swapchains;
};
struct VkInstance_T {
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    struct VkPhysicalDevice_T physical;
    VkBool32 features2_extension_enabled;
    VkBool32 device_group_creation_enabled;
    VkBool32 surface_extension_enabled;
    VkBool32 display_extension_enabled;
    struct VkSurfaceKHR_T *surfaces;
    /* Instance-level API version in use: the lower of the application's
     * request and PS5VK_INSTANCE_API_VERSION. The device reports its own. */
    uint32_t api_version;
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
    /* Enabled only for a device with a native presentation backend. */
    VkBool32 swapchain_extension_enabled;
    struct VkSwapchainKHR_T *swapchains;
    VkDeviceSize buffer_alignment;
    VkDeviceSize uniform_buffer_alignment;
    VkDeviceSize noncoherent_atom;
    VkDeviceSize max_allocation;
    uint32_t enabled_features;
    uint32_t enabled_features_t09;
    VkBool32 device_group_extension_enabled;
    /* VK_KHR_create_renderpass2 was enabled on this device. The KHR render
     * pass 2 entry points refuse, and the proc-address lookup hides them,
     * unless it was. */
    VkBool32 create_renderpass2_extension_enabled;
    /* VK_KHR_descriptor_update_template was enabled on this device; its
     * three KHR commands are visible only then. vkCreateDevice does not
     * enumerate or accept the extension until the native capability probe
     * is re-measured with it. */
    VkBool32 descriptor_update_template_extension_enabled;
    /* VK_KHR_maintenance2 was enabled on this device: the structures it
     * defines are accepted only then. */
    VkBool32 maintenance2_extension_enabled;
    /* VK_EXT_transform_feedback was enabled on this device: its buffer usages
     * and entry points exist only then. The transformFeedback feature itself
     * is PS5VK_T09_FEATURE_TRANSFORM_FEEDBACK in enabled_features_t09, and
     * geometryStreams (a stream other than zero) is recorded separately. */
    VkBool32 transform_feedback_extension_enabled;
    VkBool32 geometry_streams_enabled;
    /* VK_KHR_get_memory_requirements2, VK_KHR_dedicated_allocation and
     * VK_KHR_bind_memory2 were enabled on this device: their KHR commands and
     * structures are reachable only then. */
    VkBool32 memory_requirements2_extension_enabled;
    VkBool32 dedicated_allocation_extension_enabled;
    VkBool32 bind_memory2_extension_enabled;
    /* VK_KHR_maintenance4 was enabled: vkGetDevice*MemoryRequirementsKHR. */
    VkBool32 maintenance4_extension_enabled;
    /* VK_EXT_extended_dynamic_state: the extension (which exposes its
     * commands) and its extendedDynamicState feature (which lets the setters
     * record and pipelines declare its dynamic states). */
    VkBool32 extended_dynamic_state_extension_enabled;
    /* VK_KHR_maintenance1 was enabled: a viewport may have a negative height. */
    VkBool32 maintenance1_extension_enabled;
    /* VK_KHR_copy_commands2 was enabled on this device. */
    VkBool32 copy_commands2_extension_enabled;
    /* VK_KHR_depth_stencil_resolve and VK_KHR_dynamic_rendering: the
     * extensions, and the dynamicRendering feature that lets commands and
     * pipelines use them. */
    VkBool32 depth_stencil_resolve_extension_enabled;
    VkBool32 dynamic_rendering_extension_enabled;
    VkBool32 dynamic_rendering_enabled;
    VkBool32 extended_dynamic_state_enabled;
    /* VK_KHR_image_format_list was enabled on this device: vkCreateImage
     * accepts VkImageFormatListCreateInfo only then. */
    VkBool32 image_format_list_extension_enabled;
    /* The platform serves the mutable-format reinterpretations of
     * PS5VK_T09_FEATURE_IMAGE_FORMAT_LIST. VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT
     * is core Vulkan 1.0, so this does not depend on the extension; a
     * hand-built device leaves it zero and refuses the flag. */
    VkBool32 mutable_format_views;
    /* VK_KHR_synchronization2 was enabled on this device: its six KHR
     * commands are reachable only then (the feature bit itself is in
     * enabled_features_t09). */
    VkBool32 synchronization2_extension_enabled;
    /* The capability mask the platform reported when this device was created.
     * State that is not a Vulkan feature the application enables - the sample
     * counts a framebuffer may use, for one - is gated on this mask, so the
     * object frontends and the physical limits are derived from one source
     * (DXVK262-T06). A hand-built device leaves it zero, which is the
     * single-sample baseline. */
    uint32_t platform_features;
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
    /* VK_KHR_timeline_semaphore was enabled on this device. */
    VkBool32 timeline_extension_enabled;
    /* Serializes queue progression, submission, timeline payloads and fence
     * state between threads: vkSignalSemaphoreKHR, vkWaitSemaphoresKHR and
     * vkGetSemaphoreCounterValueKHR may run concurrently with the queue.
     * vkCreateDevice initializes it and vkDestroyDevice destroys it. Host
     * tests that hand-build a zeroed device rely on the host C library, where
     * an all-zero mutex is the default static initializer. */
    pthread_mutex_t queue_lock;
    /* The inlineUniformBlock feature was enabled on this device. Descriptor
     * set layouts and pools accept VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK
     * only then. vkCreateDevice never sets it while the feature is
     * unreported (DXVK262-T12). */
    VkBool32 inline_uniform_block_enabled;
};

void ps5vk_device_enable_runtime_compiler(VkDevice device);

/* A blocking mutex, not a spin lock. The locked regions can be long: they
 * start queue work, which runs native prepare and launch (the native launch
 * waits for GPU completion and may sleep for up to its timeout), frontend
 * copies, diagnostic logging and allocator callbacks. Waiters therefore sleep
 * in the kernel instead of spinning against a fixed-priority holder. Queue
 * waits release it around every progress pause. It is not recursive: no
 * locked region calls back into a locking entry point. */
static inline void ps5vk_device_lock(VkDevice device)
{ (void)pthread_mutex_lock(&device->queue_lock); }
static inline void ps5vk_device_unlock(VkDevice device)
{ (void)pthread_mutex_unlock(&device->queue_lock); }

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
