#ifndef PS5VK_VK_INTERNAL_H
#define PS5VK_VK_INTERNAL_H

#include <vulkan/vulkan_core.h>
#include <stddef.h>

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
struct ps5vk_native_memory_budget { uint64_t limit, used; };
void ps5vk_native_queue_configure(VkDevice device);
struct ps5vk_compiled_program;
struct ps5vk_graphics_key;
enum ps5vk_feature_bits {
    PS5VK_FEATURE_STORAGE_BUFFER_8BIT = 1u << 0,
    PS5VK_FEATURE_STORAGE_BUFFER_16BIT = 1u << 1,
};
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
    VkResult (*graphics_create)(VkDevice, const void *program_data, void **owned_state);
    void (*graphics_release)(VkDevice, void *owned_state);
    VkResult (*image_requirements)(VkDevice, const VkImageCreateInfo *, VkMemoryRequirements *);
    struct VkImage_T *images;
    struct ps5vk_compiler compiler;
    struct ps5vk_compilation_cache *pipeline_cache;
    VkBool32 runtime_compiler_enabled;
    struct VkCommandPool_T *command_pools;
    VkBool32 (*invalidate)(VkDevice, VkObjectType, const void *);
    struct VkFence_T *fences;
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
VkResult ps5vk_buffer_span(VkDevice device, VkBuffer buffer, VkDeviceSize offset,
                          VkDeviceSize range, void **address, VkDeviceSize *size);

VkBool32 ps5vk_buffer_usage(VkDevice,VkBuffer,VkBufferUsageFlags);
#endif
