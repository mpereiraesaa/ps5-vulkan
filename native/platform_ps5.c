#include "vk_internal.h"
#include "vk_pipeline.h"
#ifdef PS5VK_NO_OFFLINE_LIBRARY
static struct ps5vk_program_library ps5vk_compiled_library={0};
#else
#include "program_library.h"
#endif
#include "ps5_platform.h"
#include "ps5_agc.h"
#include "ps5log.h"
#include "graphics_formats.h"
#include "physical_device_profile.h"
#include "device_profile_report.h"
#include "vk_queue.h"
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#ifdef PS5VK_GRAPHICS_API
#if !defined(PS5VK_RUNTIME_GRAPHICS) || !PS5VK_RUNTIME_GRAPHICS
#include "graphics_library.h"
#endif
#include "graphics_pipeline_ps5.h"
#if defined(PS5VK_RUNTIME_GRAPHICS) && PS5VK_RUNTIME_GRAPHICS
#include "runtime_graphics_compiler.h"
#include "compilation_cache.h"
static struct ps5vk_compilation_cache *graphics_cache;
static VkResult acquire_graphics(void *context,const struct ps5vk_graphics_key *key,const void **out)
{
    struct ps5vk_cache_stats before={0},after={0};
    ps5vk_compilation_cache_get_stats(context,&before);
    VkResult rc=ps5vk_runtime_graphics_cached_acquire(context,key,out);
    ps5vk_compilation_cache_get_stats(context,&after);
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_RUNTIME_GRAPHICS_CACHE rc=%d hit=%u compiled_pairs=%llu hits=%llu misses=%llu entries=%u bytes=%llu",
        rc,(unsigned)(after.hits>before.hits),(unsigned long long)after.compiles,
        (unsigned long long)after.hits,(unsigned long long)after.misses,
        after.current_entries,(unsigned long long)after.current_bytes);
    return rc;
}
#endif
void ps5vk_native_graphics_queue_configure(VkDevice);
#endif
#if defined(PS5VK_RUNTIME_COMPILER) && PS5VK_RUNTIME_COMPILER
#include "ps5vk_compiler.h"
#endif

/* A deliberately bounded implementation heap, NOT measured physical RAM or
 * Vulkan conformance limits. Memory allocations and internal shader arenas
 * share the budget; the separate command mapping adds 128 KiB while pending. */
#ifdef PS5VK_GRAPHICS_API
/* Room for presentation allocations plus pipeline/state objects. This is a
 * project budget, not a claim about the console's available memory. */
#define HEAP_BYTES (UINT64_C(256) * 1024 * 1024)
#else
#define HEAP_BYTES (UINT64_C(64) * 1024 * 1024)
#endif
static struct ps5vk_native_memory_budget budget = {HEAP_BYTES, 0};
/* AGC and its direct-memory budget are process resources, while Vulkan permits
 * more than one logical device for a physical device.  Serialize the short
 * process-session transitions and keep the native module alive until the last
 * VkDevice releases it. */
static atomic_flag session_lock = ATOMIC_FLAG_INIT;
static unsigned opened;
static void lock_session(void)
{
    while (atomic_flag_test_and_set_explicit(&session_lock, memory_order_acquire)) {}
}
static void unlock_session(void)
{
    atomic_flag_clear_explicit(&session_lock, memory_order_release);
}
static void retain(const char *reason)
{
    ps5log_printf(PS5LOG_ERR, "PS5VK_PLATFORM_RETAIN reason=%s", reason);
    ps5log_close("platform-retained");
    for (;;) sleep(1);
}
static VkResult open_backend(void *unused, struct ps5vk_memory_backend *memory)
{
    (void)unused;
    lock_session();
    if (opened) {
        ++opened;
        *memory =
#ifdef PS5VK_GRAPHICS_API
            ps5vk_native_graphics_memory_backend();
#else
            ps5vk_native_memory_backend();
#endif
        memory->context = &budget;
        ps5log_printf(PS5LOG_INFO, "PS5VK_PLATFORM_JOIN users=%u", opened);
        unlock_session();
        return VK_SUCCESS;
    }
    int rc = sceSysmoduleLoadModuleInternal(0x80000094u);
    ps5log_printf(PS5LOG_INFO, "PS5VK_PLATFORM_LOAD rc=%d", rc);
    if (rc) {
        unlock_session();
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t state = 0;
    rc = sceAgcInit(&state, sizeof(state));
    ps5log_printf(PS5LOG_INFO, "PS5VK_PLATFORM_INIT rc=%d", rc);
    if (rc) {
        if (sceSysmoduleUnloadModuleInternal(0x80000094u)) retain("init-rollback-unload");
        unlock_session();
        return VK_ERROR_INITIALIZATION_FAILED;
    }
#if defined(PS5VK_RUNTIME_GRAPHICS) && PS5VK_RUNTIME_GRAPHICS
    graphics_cache=ps5vk_compilation_cache_create(32,4u*1024u*1024u);
    if(!graphics_cache) {
        if(sceSysmoduleUnloadModuleInternal(0x80000094u))retain("cache-rollback-unload");
        unlock_session();
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
#endif
    opened = 1;
    *memory =
#ifdef PS5VK_GRAPHICS_API
        ps5vk_native_graphics_memory_backend();
#else
        ps5vk_native_memory_backend();
#endif
    memory->context = &budget;
    unlock_session();
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *memory)
{
    lock_session();
    if (!opened || memory->context != &budget) {
        unlock_session();
        retain("invalid-session-close");
    }
    if (opened > 1) {
        --opened;
        memset(memory, 0, sizeof(*memory));
        ps5log_printf(PS5LOG_MARK, "PS5VK_PLATFORM_LEAVE users=%u", opened);
        unlock_session();
        return;
    }
    if (budget.used) {
        unlock_session();
        retain("live-memory-on-close");
    }
#if defined(PS5VK_RUNTIME_GRAPHICS) && PS5VK_RUNTIME_GRAPHICS
    ps5vk_compilation_cache_destroy(graphics_cache);graphics_cache=NULL;
    ps5log_line(PS5LOG_MARK,"PS5VK_RUNTIME_GRAPHICS_CACHE_DESTROYED");
#endif
#if defined(PS5VK_KEEP_AGC_MODULE) && PS5VK_KEEP_AGC_MODULE
    /* Diagnostic only: leave the module reference for process termination.
     * Do not report this as a successful explicit unload. */
    ps5log_printf(PS5LOG_MARK,"PS5VK_PLATFORM_MODULE_RETAINED diagnostic=1 allocations_bytes=%llu",
                  (unsigned long long)budget.used);
#else
    int rc = sceSysmoduleUnloadModuleInternal(0x80000094u);
    ps5log_printf(PS5LOG_MARK, "PS5VK_PLATFORM_CLOSE rc=%d allocations_bytes=%llu", rc,
                  (unsigned long long)budget.used);
    if (rc) retain("unload");
#endif
    opened = 0; memset(memory, 0, sizeof(*memory));
    unlock_session();
}
static void configure(VkDevice d)
{
    ps5vk_native_queue_configure(d);
#if defined(PS5VK_RUNTIME_COMPILER) && PS5VK_RUNTIME_COMPILER
    d->runtime_compiler_enabled = VK_TRUE;
#endif
#ifdef PS5VK_GRAPHICS_API
    /* Experimental graphics objects; real submission is separately enabled by
     * PS5VK_GRAPHICS_DRAW. A complete graphics queue profile is not yet advertised. */
    d->graphics_enabled = VK_TRUE;
    d->graphics_release = ps5vk_native_graphics_release;
#if defined(PS5VK_RUNTIME_GRAPHICS) && PS5VK_RUNTIME_GRAPHICS
    d->graphics_library=NULL;
    d->graphics_compiler_context=graphics_cache;
    d->graphics_acquire=acquire_graphics;
    d->graphics_compiled_release=ps5vk_runtime_graphics_cached_release;
    d->graphics_create=ps5vk_native_runtime_graphics_create;
#else
    d->graphics_library = &graphics_library;
    d->graphics_create = ps5vk_native_graphics_create;
#endif
    d->image_requirements = ps5vk_native_image_requirements;
#if PS5VK_GRAPHICS_DRAW
    struct ps5vk_queue_backend compute=d->submit_backend;
    ps5vk_native_graphics_queue_configure(d);
    ps5vk_queue_router_configure(d,compute,d->submit_backend);
#endif
#endif
}
VkResult ps5vk_platform_query(struct ps5vk_platform *platform)
{
    memset(platform, 0, sizeof(*platform));
    platform->open = open_backend; platform->close = close_backend;
    platform->configure = configure;
    platform->queue_flags = VK_QUEUE_COMPUTE_BIT;
#if defined(PS5VK_GRAPHICS_API) && PS5VK_GRAPHICS_DRAW
    platform->queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    platform->format_properties = ps5vk_graphics_format_properties;
    platform->image_properties = ps5vk_graphics_image_properties;
#endif
#if defined(PS5VK_RUNTIME_COMPILER) && PS5VK_RUNTIME_COMPILER
    platform->compiler = (struct ps5vk_compiler){&ps5vk_compiled_library, ps5vk_program_resolve, ps5vk_compiler_adapter_compile};
    platform->supported_features = PS5VK_FEATURE_STORAGE_BUFFER_8BIT |
                                   PS5VK_FEATURE_STORAGE_BUFFER_16BIT |
                                   PS5VK_FEATURE_ROBUST_BUFFER_ACCESS;
#if defined(PS5VK_GRAPHICS_API) && PS5VK_GRAPHICS_DRAW
    /* The graphics runtime path delivers the draw-parameter built-ins for the
     * direct and single-indirect contract this profile witnessed. */
    platform->supported_features |= PS5VK_FEATURE_SHADER_DRAW_PARAMETERS;
#endif
    /* Multiview is the capability the private six-view and instance witnesses
     * measured on this console. The bit is internal for now: E1a advertises
     * nothing, and the slice that enumerates the extension will do so from
     * here together with the queries and the device negotiation. */
    platform->supported_features |= PS5VK_FEATURE_MULTIVIEW;
#else
    platform->compiler = (struct ps5vk_compiler){&ps5vk_compiled_library, ps5vk_program_resolve, NULL};
    platform->supported_features = PS5VK_FEATURE_ROBUST_BUFFER_ACCESS;
#endif
    platform->max_allocation = HEAP_BYTES;
    /* The same initializer the host reporting dump uses; see
     * src/device_profile_report.h. Object-model sizing follows
     * PS5VK_GRAPHICS_API, the advertised graphics limit/format matrix follows
     * PS5VK_GRAPHICS_DRAW. */
#ifdef PS5VK_GRAPHICS_API
    const int graphics_objects = VK_TRUE;
#else
    const int graphics_objects = VK_FALSE;
#endif
#if defined(PS5VK_GRAPHICS_API) && PS5VK_GRAPHICS_DRAW
    const int graphics_submit = VK_TRUE;
#else
    const int graphics_submit = VK_FALSE;
#endif
    ps5vk_device_profile_init(&platform->properties, &platform->memory_properties,
        graphics_objects, graphics_submit);
    return VK_SUCCESS;
}
