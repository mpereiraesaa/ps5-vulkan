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
#include "tess_profile.h"
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

/* A deliberately bounded implementation heap, NOT measured physical RAM.
 * Memory allocations and internal shader arenas share the budget; the
 * separate command mapping adds 128 KiB while pending. The values live in
 * src/device_profile_report.h so the host reporting dump reads the same ones. */
#ifdef PS5VK_GRAPHICS_API
/* One 1 GiB allocation plus headroom for presentation and pipeline/state
 * objects. A project budget, not a claim about the console's memory. */
#define HEAP_BYTES PS5VK_PROFILE_GRAPHICS_HEAP_BYTES
#define MAX_ALLOCATION_BYTES PS5VK_PROFILE_GRAPHICS_MAX_ALLOCATION_BYTES
#else
#define HEAP_BYTES PS5VK_PROFILE_COMPUTE_HEAP_BYTES
#define MAX_ALLOCATION_BYTES PS5VK_PROFILE_COMPUTE_HEAP_BYTES
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
    d->graphics_used_sets=ps5vk_native_graphics_used_sets;
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
                                   PS5VK_FEATURE_ROBUST_BUFFER_ACCESS |
                                   PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT;
#if defined(PS5VK_GRAPHICS_API) && PS5VK_GRAPHICS_DRAW
    /* The graphics runtime path delivers the draw-parameter built-ins for the
     * direct and single-indirect contract this profile witnessed. */
    platform->supported_features |= PS5VK_FEATURE_SHADER_DRAW_PARAMETERS;
    /* Multiview belongs to the same measured graphics path and nowhere else:
     * the private six-view and instance witnesses ran through exactly this
     * build, so a runtime-compiler build WITHOUT the graphics API must not
     * report itself multiview-capable - it cannot execute a render pass or a
     * draw at all. The public KHR query exposes this bit only on that path. */
    platform->supported_features |= PS5VK_FEATURE_MULTIVIEW;
    /* DXVK262-T03: indirect firstInstance, multi-draw expansion with the
     * command index as DrawIndex, and the full 32-bit index range execute on
     * exactly this graphics/runtime-compiler path (vk_indirect.c,
     * graphics_queue_ps5.c, index_fetch.c); the public consumer witness and
     * the upstream draw leaves that measure them ran through this build. */
    platform->supported_features |= PS5VK_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE |
                                    PS5VK_FEATURE_MULTI_DRAW_INDIRECT |
                                    PS5VK_FEATURE_FULL_DRAW_INDEX_UINT32;
    /* User-defined clip and cull distances: the pre-raster export (static and
     * dynamically indexed) and the fragment stage's read of the interpolated
     * value are native-witnessed on exactly this build - the eleven-case
     * clip/cull witness, private-captures/t04/clip-cull-pixel-read-acceptance -
     * and the profile reports the three distance limits at the Vulkan floor of
     * eight, which is the width of the two packed position registers the
     * interface policy bounds a declaration against. */
    platform->supported_features |= PS5VK_FEATURE_SHADER_CLIP_DISTANCE |
                                    PS5VK_FEATURE_SHADER_CULL_DISTANCE;
    /* The optional geometry stage, on the same measured graphics path: the
     * nineteen-case geometry witness verifies the merged pre-raster program end
     * to end - the ES->GS handoff it reads, the triangle, point and line input
     * families under their own input assemblies, gl_InvocationID, the
     * per-primitive id, and the five mandatory minima the profile reports in
     * src/graphics_limits.h - so this build is the one that may advertise it.
     * The runtime graphics cache and the interface policy are what refuse a
     * geometry pipeline on any build without this bit. */
    platform->supported_features |= PS5VK_FEATURE_GEOMETRY_SHADER;
    /* DXVK262-T05, promoted 2026-09-21 on physical-console evidence. The four
     * rasterization and viewport features are advertised by the shipping
     * build: each executes through the public ABI (the consumer's raster and
     * viewport witnesses) and each has its applicable upstream leaves passing
     * in the frozen acceptance selection - depthClamp all eight, depthBiasClamp
     * its only two, multiViewport all twenty-two, fillModeNonSolid all
     * twenty-eight it can run. Its twenty-ninth, the amber line-continuity
     * leaf, is not runnable on this profile for a reason that has nothing to
     * do with the feature: Amber demands host-coherent memory this device does
     * not advertise (cts/upstream/manifest.json, host-coherent-memory-gap).
     * The measurement guard that carried these bits before the promotion is
     * gone; nothing else in this file sets them. */
    platform->supported_features |= PS5VK_FEATURE_DEPTH_BIAS_CLAMP |
                                    PS5VK_FEATURE_DEPTH_CLAMP |
                                    PS5VK_FEATURE_FILL_MODE_NON_SOLID |
                                    PS5VK_FEATURE_MULTI_VIEWPORT;
    /* DXVK262-T06 sampleRateShading, promoted on 2026-09-23. The row's four
     * axes are measured: the device reports and accepts the feature, the pixel
     * stage publishes the sample positions, the position at the iterated
     * sample and the synchronous colour-to-texture barrier its leaves need, and
     * the feature's own oracle passes. The thirty leaves whose own checkSupport
     * requires the feature and whose shapes this profile renders are the
     * sample-rate-shading acceptance group (5 minSampleShading values x 2
     * served counts x the triangle and quad shapes), measured Pass three times
     * in a row; the twenty point/line shapes the same oracle selects are
     * refused by this profile's pipeline resolver, not by this feature, and
     * stay diagnostics next to the rest of those refusals. The measurement
     * switch that carried this bit before the promotion
     * (PS5VK_SAMPLE_RATE_DIAGNOSTIC) no longer guards it, and nothing else in
     * this file sets it. */
    platform->supported_features |= PS5VK_FEATURE_SAMPLE_RATE_SHADING;
    /* DXVK262-T06 fragment storage side effects.  The public-SDK witness
     * distinguishes a zero-write control from exactly 4096 fragment writes,
     * preserves 30 guard words and completes its fence.  The two unchanged
     * upstream frag_side_effects kill leaves then pass in the same 306-case
     * run as the frozen 304-case regression set.  This shipping bit is the
     * evidence boundary: builds without this exact path still report false
     * and vkCreateDevice rejects a request for the feature. */
    platform->supported_features |= PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS;
    /* DXVK262-T06 dual-source blending, promoted on 2026-09-21. The public-SDK
     * witness renders the packaged two-output fragment module twice - blending
     * disabled against the accepted SRC1 equation - and judges both reads on
     * exact bytes (control 64,128,191,255 inside one LSB, candidate
     * 51,51,38,255 exactly) with the two required to differ, so a blender that
     * ignored the secondary export cannot pass. The 98 applicable upstream
     * blend.dual_source leaves then passed in one 404-case run together with
     * the 306-case acceptance selection. This bit is the evidence boundary:
     * the front end and the compiler still refuse a SRC1 equation without it,
     * and without the proven secondary export. */
    platform->supported_features |= PS5VK_FEATURE_DUAL_SRC_BLEND;
    /* DXVK262-T06 independentBlend, promoted 2026-09-22. The two-colour-target
     * path is built and measured end to end: the private two-MRT witness
     * (src/two_mrt_oracle.c, native/two_mrt_probe.c) showed one draw writing
     * two attachments with different values, and the four upstream leaves that
     * require the feature now run on hardware - the two
     * suballocation.attachment_write_mask leaves pass, and the two
     * dedicated_allocation ones stay out because they need
     * VK_KHR_dedicated_allocation, which this profile does not advertise. The
     * advertised maxColorAttachments moves with it in src/graphics_limits.h;
     * the front end, the pipeline key and the native per-target programming
     * already carry the bound. */
    platform->supported_features |= PS5VK_FEATURE_INDEPENDENT_BLEND;
    /* Two-cube GPU readback and the applicable upstream case qualify this bit.
     * Unsupported tiled layer pitches still fail at descriptor creation. */
    platform->supported_features |= PS5VK_FEATURE_IMAGE_CUBE_ARRAY;
    /* BC sampling, filtering and transfer roles passed original CTS;
     * format queries expose only implemented roles. */
    platform->supported_features |= PS5VK_FEATURE_TEXTURE_COMPRESSION_BC;
    /* Precise occlusion has native counter and original CTS evidence. */
    platform->supported_features |= PS5VK_FEATURE_OCCLUSION_QUERY_PRECISE;
    /* Constant, dynamic, four-offset and Dref forms have GPU readback and
     * original CTS coverage at the required offset limits. */
    platform->supported_features |= PS5VK_FEATURE_SHADER_IMAGE_GATHER_EXTENDED;

    /* Completed precise occlusion queries and host reset/reuse have a strict
     * SDK-linked native witness. Expose the Vulkan 1.0 EXT feature route. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_HOST_QUERY_RESET;
#if defined(PS5VK_IMAGELESS_FRAMEBUFFER_DIAGNOSTIC) && PS5VK_IMAGELESS_FRAMEBUFFER_DIAGNOSTIC
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_IMAGELESS_FRAMEBUFFER;
#endif
#if defined(PS5VK_DESCRIPTOR_UPDATE_TEMPLATE_DIAGNOSTIC) && PS5VK_DESCRIPTOR_UPDATE_TEMPLATE_DIAGNOSTIC
    /* Measurement only: VK_KHR_descriptor_update_template, until the native
     * capability probe is re-measured with it enumerated. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_DESCRIPTOR_UPDATE_TEMPLATE;
#endif
#if defined(PS5VK_ROBUSTNESS2_DIAGNOSTIC) && PS5VK_ROBUSTNESS2_DIAGNOSTIC
    /* DXVK262-T13 measurement build only: VK_EXT_robustness2 with
     * robustBufferAccess2 and nullDescriptor, so the public-SDK witness can
     * negotiate both before the shipping profile reports them. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2 |
        PS5VK_T09_FEATURE_NULL_DESCRIPTOR;
#endif
    /* VkFormatProperties3 (VK_KHR_format_feature_flags2) and the RGBA8
     * UNORM <-> SRGB mutable views with VK_KHR_image_format_list: the
     * SDK-linked mutable-view witness sampled the SRGB view of the UNORM
     * image exactly as a native SRGB image. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_FORMAT_FEATURE_FLAGS2 |
        PS5VK_T09_FEATURE_IMAGE_FORMAT_LIST;
    /* Nearest/linear U, V and W SDK readback plus the compact original 3D
     * address-mode CTS leaf support the Vulkan 1.0 KHR extension route. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE;

    /* separateDepthStencilLayouts (DXVK262-T09) and the Vulkan 1.0 extension
     * route the registry requires for it: VK_KHR_maintenance2 and
     * VK_KHR_create_renderpass2 over VK_KHR_multiview. Per-aspect layout
     * state, the two D32_SFLOAT_S8_UINT planes and per-aspect barriers,
     * load/store and readback passed the public-SDK witness and the focused
     * original stencil/depth leaves. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_SEPARATE_DEPTH_STENCIL_LAYOUTS |
        PS5VK_T09_FEATURE_MAINTENANCE2 | PS5VK_T09_FEATURE_CREATE_RENDERPASS2;
    /* DXVK262-T11 pixel removal on the Vulkan 1.0 profile. The SDK-linked
     * pixel-removal witness showed OpKill, OpTerminateInvocation and
     * OpDemoteToHelperInvocation each keep every removed pixel out of the
     * depth and stencil planes, and the helper-invocation witness showed every
     * demote spelling (SPIR-V 1.6 core without the extension declaration, as
     * DXVK emits it, 1.6 with it, and the 1.3 EXT form) keeps the removed lane
     * running as a helper: every derivative its quad read afterwards was
     * exact. Derivatives after OpTerminateInvocation were not, which the
     * specification leaves undefined. */
    platform->supported_features_t09 |=
        PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION |
        PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION;
#if defined(PS5VK_DXVK_RENDER_DIAGNOSTIC) && PS5VK_DXVK_RENDER_DIAGNOSTIC
    /* Private measurement build (DXVK262-T10): report the DXVK first-draw
     * recording routes so the witness negotiates them through the public API
     * before any shipping platform advertises them. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_EXTENDED_DYNAMIC_STATE |
        PS5VK_T09_FEATURE_COPY_COMMANDS2 | PS5VK_T09_FEATURE_DEPTH_STENCIL_RESOLVE |
        PS5VK_T09_FEATURE_DYNAMIC_RENDERING | PS5VK_T09_FEATURE_MAINTENANCE1;
#endif
    /* VK_KHR_synchronization2: vkCmdPipelineBarrier2, vkQueueSubmit2 and
     * the event commands convert onto the Vulkan 1.0 barrier and submit
     * routes; the SDK-linked sync2 witness executed all three phases with
     * zero mismatches. Timestamp2 stays refused with the other timestamps. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_SYNCHRONIZATION2;

#endif
#else
    platform->compiler = (struct ps5vk_compiler){&ps5vk_compiled_library, ps5vk_program_resolve, NULL};
    platform->supported_features = PS5VK_FEATURE_ROBUST_BUFFER_ACCESS;
#endif
    /* The Vulkan 1.0 KHR route is backed by the seven unchanged volatile
     * queue-family atomic CTS leaves and the bounded GPU ordering witness. */
    platform->supported_features |= PS5VK_FEATURE_VULKAN_MEMORY_MODEL;
    /* DeviceScope is exposed through the KHR feature chain under Vulkan 1.0.
     * The original message-passing CTS factory requires core Vulkan 1.1, so
     * the KHR route has a separate bounded native witness. */
    platform->supported_features |= PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE;
#if defined(PS5VK_SHADER_INT16_DIAGNOSTIC) && PS5VK_SHADER_INT16_DIAGNOSTIC
    /* Measure the core Int16 route with original CTS before considering the
     * shader feature for the ordinary profile. Subgroup bits stay separate. */
    platform->supported_features |= PS5VK_FEATURE_SHADER_INT16;
#endif
#if defined(PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC) && PS5VK_SUBGROUP_BROADCAST_DIAGNOSTIC
    /* Private Vulkan 1.0 measurement only: admit compute Ballot Broadcast
     * through the runtime pipeline without reporting subgroup properties or
     * either T08 subgroup feature. Public API eligibility remains blocked. */
    platform->supported_features |= PS5VK_FEATURE_SUBGROUP_BROADCAST_COMPUTE;
#endif
#if defined(PS5VK_SUBGROUP_IADD_DIAGNOSTIC) && PS5VK_SUBGROUP_IADD_DIAGNOSTIC
    /* Private compute IAdd measurement only. This is narrower than the
     * public ARITHMETIC operation bit and reports no subgroup properties. */
    platform->supported_features |= PS5VK_FEATURE_SUBGROUP_IADD_COMPUTE;
#endif
#if defined(PS5VK_SUBGROUP_BASIC_DIAGNOSTIC) && PS5VK_SUBGROUP_BASIC_DIAGNOSTIC
    /* Private compute subgroup BASIC measurement only (Elect, barriers,
     * built-ins); subgroup properties and features stay unreported. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_SUBGROUP_BASIC_COMPUTE;
#endif
#if defined(PS5VK_SHADER_INT8_DIAGNOSTIC) && PS5VK_SHADER_INT8_DIAGNOSTIC
    /* Compiler-only probe; public shaderInt8 and subgroup features stay false. */
    platform->supported_features |= PS5VK_FEATURE_SHADER_INT8_COMPUTE;
#endif
    /* Vulkan 1.0 exposes the KHR route through device-group creation and
     * properties2. The bounded address witness and two unchanged original
     * buffer-address compute leaves execute through the R32_UINT output. */
    platform->supported_features |= PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS;
    /* VK_KHR_timeline_semaphore (DXVK262-T09). The payload lives in the
     * queue frontend and advances only when a record retires after this
     * backend's exact-serial completion; maxTimelineSemaphoreValueDifference
     * is UINT64_MAX because every comparison is full-width. Promoted on the
     * public-SDK witness and the focused original timeline leaves. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_TIMELINE_SEMAPHORE;
    platform->max_allocation = MAX_ALLOCATION_BYTES;
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
        graphics_objects, graphics_submit, platform->supported_features);
    ps5vk_native_tess_profile(platform);
#if defined(PS5VK_DXVK_ROUTES_DIAGNOSTIC) && PS5VK_DXVK_ROUTES_DIAGNOSTIC
    /* Private measurement build only (DXVK262): report the memory-requirement
     * and binding routes DXVK calls right after device creation
     * (VK_KHR_get_memory_requirements2, VK_KHR_dedicated_allocation,
     * VK_KHR_bind_memory2), so the native DXVK payload can reach its first
     * draw before a witness promotes them. The render and format routes have
     * their own switch (PS5VK_DXVK_RENDER_DIAGNOSTIC); the format routes are
     * shipping. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_GET_MEMORY_REQUIREMENTS2 |
        PS5VK_T09_FEATURE_DEDICATED_ALLOCATION | PS5VK_T09_FEATURE_BIND_MEMORY2;
#endif
#if defined(PS5VK_MAINTENANCE4_DIAGNOSTIC) && PS5VK_MAINTENANCE4_DIAGNOSTIC
    /* DIAGNOSTIC DXVK measurement only, never shipping: VK_KHR_maintenance4
     * requires a Vulkan 1.1 device, which this profile does not report. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_MAINTENANCE4;
    platform->maintenance4_diagnostic_on_vulkan_1_0 = VK_TRUE;
#endif
#if defined(PS5VK_HOST_COHERENT_DIAGNOSTIC) && PS5VK_HOST_COHERENT_DIAGNOSTIC
    /* Witness-only: the driver-maintained HOST_COHERENT type is not part of
     * the ordinary profile until its native witness passes. */
    platform->supported_features_t09 |= PS5VK_T09_FEATURE_HOST_COHERENT_MEMORY;
    ps5vk_profile_add_coherent_type(&platform->memory_properties);
#endif
    return VK_SUCCESS;
}
