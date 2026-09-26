/* VK_EXT_transform_feedback on the Vulkan 1.0 profile (DXVK262-T14):
 * extension enumeration, the Features2/Properties2 chains, device enablement
 * and its registry dependency, device-level lookup, buffer usages, barrier
 * scopes, and the recording rules of bind/begin/end. Only platform discovery
 * is mocked; the render pass and capture pipeline are hand-built objects so the
 * recording rules can be exercised without a graphics backend. */
#include "vk_internal.h"
#include "vk_command.h"
#include "vk_query_pool.h"
#include "vk_render_pass.h"
#include "vk_indirect.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint32_t platform_features_t09;
static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = calloc(1, size); *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, cache, cache};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
        .max_allocation = 65536, .queue_flags = VK_QUEUE_COMPUTE_BIT,
        .supported_features = PS5VK_FEATURE_ROBUST_BUFFER_ACCESS,
        .supported_features_t09 = platform_features_t09};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU", .heap_size = 65536,
        .allocation_granularity = 1, .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

/* features2: 1 enables the instance extension, 0 is a Vulkan 1.0 instance
 * without it, 2 asks for an API 1.1 instance without it. */
static VkInstance make_instance(int features2)
{
    const char *extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = features2 == 2 ? &app : NULL,
        .enabledExtensionCount = features2 == 1 ? 1u : 0u, .ppEnabledExtensionNames = &extension};
    VkInstance i = VK_NULL_HANDLE;
    assert(vkCreateInstance(&info, NULL, &i) == VK_SUCCESS);
    return i;
}
static VkPhysicalDevice physical(VkInstance i)
{
    uint32_t count = 1;
    VkPhysicalDevice p = VK_NULL_HANDLE;
    assert(vkEnumeratePhysicalDevices(i, &count, &p) == VK_SUCCESS && p);
    return p;
}
static int lists_extension(VkPhysicalDevice p, uint32_t *spec)
{
    uint32_t count = 0;
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, NULL) == VK_SUCCESS);
    VkExtensionProperties properties[32];
    assert(count <= 32);
    assert(vkEnumerateDeviceExtensionProperties(p, NULL, &count, properties) == VK_SUCCESS);
    for (uint32_t n = 0; n < count; ++n)
        if (!strcmp(properties[n].extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) {
            if (spec) *spec = properties[n].specVersion;
            return 1;
        }
    return 0;
}
static void query(VkPhysicalDevice p, VkPhysicalDeviceTransformFeedbackFeaturesEXT *f,
                  VkPhysicalDeviceTransformFeedbackPropertiesEXT *t)
{
    *f = (VkPhysicalDeviceTransformFeedbackFeaturesEXT){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
        .transformFeedback = 2, .geometryStreams = 2};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = f};
    vkGetPhysicalDeviceFeatures2KHR(p, &features);
    memset(t, 0x5a, sizeof(*t));
    t->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT;
    t->pNext = NULL;
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = t};
    vkGetPhysicalDeviceProperties2KHR(p, &properties);
}
static VkResult create_device(VkPhysicalDevice p, int extension, const void *chain,
                              VkDevice *out)
{
    const char *name = VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = chain,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &q,
        .enabledExtensionCount = extension ? 1u : 0u, .ppEnabledExtensionNames = &name};
    *out = VK_NULL_HANDLE;
    return vkCreateDevice(p, &info, NULL, out);
}
static VkResult make_buffer(VkDevice d, VkBufferUsageFlags usage, VkDeviceSize size,
                            VkBuffer *out)
{
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = usage};
    return vkCreateBuffer(d, &bi, NULL, out);
}

static void unsupported_platform(void)
{
    platform_features_t09 = 0;
    VkInstance i = make_instance(1);
    VkPhysicalDevice p = physical(i);
    VkPhysicalDeviceTransformFeedbackFeaturesEXT f;
    VkPhysicalDeviceTransformFeedbackPropertiesEXT t;
    query(p, &f, &t);
    assert(f.transformFeedback == VK_FALSE && f.geometryStreams == VK_FALSE);
    assert(!t.maxTransformFeedbackStreams && !t.maxTransformFeedbackBuffers &&
           !t.maxTransformFeedbackBufferSize && !t.maxTransformFeedbackStreamDataSize &&
           !t.maxTransformFeedbackBufferDataSize && !t.maxTransformFeedbackBufferDataStride &&
           !t.transformFeedbackQueries && !t.transformFeedbackStreamsLinesTriangles &&
           !t.transformFeedbackRasterizationStreamSelect && !t.transformFeedbackDraw);
    assert(!lists_extension(p, NULL));
    VkDevice d;
    assert(create_device(p, 1, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    VkPhysicalDeviceTransformFeedbackFeaturesEXT request = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
        .transformFeedback = VK_TRUE};
    assert(create_device(p, 0, &request, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    /* The neutral structure enables nothing and is accepted. */
    request.transformFeedback = VK_FALSE;
    assert(create_device(p, 0, &request, &d) == VK_SUCCESS && d);
    VkBuffer b = VK_NULL_HANDLE;
    assert(make_buffer(d, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT, 64, &b) ==
           VK_ERROR_FEATURE_NOT_PRESENT && !b);
    assert(!vkGetDeviceProcAddr(d, "vkCmdBeginTransformFeedbackEXT"));
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
}

static void supported_device(void)
{
    platform_features_t09 = PS5VK_T09_FEATURE_TRANSFORM_FEEDBACK;
    /* Vulkan 1.0 without the Features2 route cannot satisfy the registry
     * dependency; an API 1.1 instance can. */
    VkInstance plain = make_instance(0);
    VkDevice d;
    assert(create_device(physical(plain), 1, NULL, &d) == VK_ERROR_EXTENSION_NOT_PRESENT && !d);
    vkDestroyInstance(plain, NULL);
    VkInstance core11 = make_instance(2);
    assert(create_device(physical(core11), 1, NULL, &d) == VK_SUCCESS && d);
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(core11, NULL);

    VkInstance i = make_instance(1);
    VkPhysicalDevice p = physical(i);
    VkPhysicalDeviceTransformFeedbackFeaturesEXT f;
    VkPhysicalDeviceTransformFeedbackPropertiesEXT t;
    query(p, &f, &t);
    uint32_t spec = 0;
    assert(f.transformFeedback == VK_TRUE && f.geometryStreams == VK_TRUE);
    assert(lists_extension(p, &spec) && spec == VK_EXT_TRANSFORM_FEEDBACK_SPEC_VERSION);
    /* Four buffers (DXVK binds slots 0..3), four streams, the compiler's data
     * envelope, and every optional behaviour false. */
    assert(t.maxTransformFeedbackStreams == 4 && t.maxTransformFeedbackBuffers == 4);
    assert(t.maxTransformFeedbackBufferSize == ((VkDeviceSize)1u << 31));
    assert(t.maxTransformFeedbackStreamDataSize == 512 &&
           t.maxTransformFeedbackBufferDataSize == 512 &&
           t.maxTransformFeedbackBufferDataStride == 2048);
    assert(t.transformFeedbackQueries && !t.transformFeedbackStreamsLinesTriangles &&
           !t.transformFeedbackRasterizationStreamSelect && t.transformFeedbackDraw);
    /* The Vulkan floors (maxTransformFeedbackBufferSize 2^27, data 512). */
    assert(t.maxTransformFeedbackBufferSize >= ((VkDeviceSize)1u << 27));

    VkPhysicalDeviceTransformFeedbackFeaturesEXT request = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
        .transformFeedback = VK_TRUE, .geometryStreams = VK_TRUE};
    /* The features need the extension; malformed and repeated structures fail. */
    assert(create_device(p, 0, &request, &d) == VK_ERROR_FEATURE_NOT_PRESENT && !d);
    request.geometryStreams = 2;
    assert(create_device(p, 1, &request, &d) == VK_ERROR_UNKNOWN && !d);
    request.geometryStreams = VK_TRUE;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT again = request;
    request.pNext = &again;
    assert(create_device(p, 1, &request, &d) == VK_ERROR_UNKNOWN && !d);
    request.pNext = NULL;

    /* Extension without the features: usages and entry points exist, the
     * capture stage in a barrier does not. */
    assert(create_device(p, 1, NULL, &d) == VK_SUCCESS && d);
    assert(d->transform_feedback_extension_enabled && !d->geometry_streams_enabled &&
           !(d->enabled_features_t09 & PS5VK_T09_FEATURE_TRANSFORM_FEEDBACK));
    assert(vkGetDeviceProcAddr(d, "vkCmdBindTransformFeedbackBuffersEXT") ==
           (PFN_vkVoidFunction)vkCmdBindTransformFeedbackBuffersEXT);
    assert(vkGetDeviceProcAddr(d, "vkCmdDrawIndirectByteCountEXT"));
    VkBuffer b;
    assert(make_buffer(d, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
                          VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 64, &b) == VK_SUCCESS);
    vkDestroyBuffer(d, b, NULL);
    vkDestroyDevice(d, NULL);

    assert(create_device(p, 1, &request, &d) == VK_SUCCESS && d);
    assert(d->transform_feedback_extension_enabled && d->geometry_streams_enabled &&
           (d->enabled_features_t09 & PS5VK_T09_FEATURE_TRANSFORM_FEEDBACK));
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(i, NULL);
}

/* A recording fixture: a device with the extension and (optionally) the
 * feature, one primary command buffer, bound capture and counter buffers, a
 * hand-built one-subpass render pass and a hand-built capture pipeline. */
struct fixture {
    VkInstance instance;
    VkDevice device;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkBuffer capture, counter, plain;
    VkDeviceMemory memory;
    struct ps5vk_subpass subpass;
    struct VkRenderPass_T pass;
    struct VkPipeline_T pipeline;
};
static void fixture_open(struct fixture *x, int feature)
{
    memset(x, 0, sizeof(*x));
    platform_features_t09 = PS5VK_T09_FEATURE_TRANSFORM_FEEDBACK;
    x->instance = make_instance(1);
    VkPhysicalDeviceTransformFeedbackFeaturesEXT request = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
        .transformFeedback = feature ? VK_TRUE : VK_FALSE};
    assert(create_device(physical(x->instance), 1, &request, &x->device) == VK_SUCCESS);
    VkDevice d = x->device;
    d->graphics_enabled = VK_TRUE;
    assert(make_buffer(d, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT, 1024,
                       &x->capture) == VK_SUCCESS);
    assert(make_buffer(d, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT |
                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, 64,
                       &x->counter) == VK_SUCCESS);
    assert(make_buffer(d, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 64, &x->plain) == VK_SUCCESS);
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 4096};
    assert(vkAllocateMemory(d, &mi, NULL, &x->memory) == VK_SUCCESS);
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(d, x->capture, &r);
    assert(vkBindBufferMemory(d, x->capture, x->memory, 0) == VK_SUCCESS);
    assert(vkBindBufferMemory(d, x->counter, x->memory, 2048) == VK_SUCCESS);
    assert(vkBindBufferMemory(d, x->plain, x->memory, 3072) == VK_SUCCESS);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    assert(vkCreateCommandPool(d, &pi, NULL, &x->pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = x->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    assert(vkAllocateCommandBuffers(d, &ai, &x->command) == VK_SUCCESS);
    x->subpass.depth.attachment = VK_ATTACHMENT_UNUSED;
    x->pass = (struct VkRenderPass_T){.device = d, .subpass_count = 1,
        .subpasses = &x->subpass};
    x->pipeline.device = d;
    x->pipeline.graphics = VK_TRUE;
    x->pipeline.xfb.buffers_mask = 1u;
    x->pipeline.xfb.captures = 1u;
    x->pipeline.xfb.strides[0] = 16u;
}
static void fixture_close(struct fixture *x)
{
    VkDevice d = x->device;
    vkDestroyCommandPool(d, x->pool, NULL);
    vkDestroyBuffer(d, x->capture, NULL);
    vkDestroyBuffer(d, x->counter, NULL);
    vkDestroyBuffer(d, x->plain, NULL);
    vkFreeMemory(d, x->memory, NULL);
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(x->instance, NULL);
}
/* Begin a fresh recording inside the hand-built pass with the capture
 * pipeline bound. Leaving the pass is done by clearing the field, so the
 * rules under test are the transform feedback ones only. */
static VkCommandBuffer record(struct fixture *x)
{
    VkCommandBuffer c = x->command;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(vkBeginCommandBuffer(c, &bi) == VK_SUCCESS);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, &x->pipeline);
    assert(c->state == PS5VK_RECORDING && c->graphics_pipeline == &x->pipeline);
    c->render_pass = &x->pass;
    c->render_pass_contents = VK_SUBPASS_CONTENTS_INLINE;
    return c;
}
static void leave(VkCommandBuffer c) { c->render_pass = NULL; }

static void recording(void)
{
    struct fixture x;
    fixture_open(&x, 1);
    const VkDeviceSize zero = 0, size = 256, eight = 8;

    /* The accepted sequence: bind, begin with a counter, end with a counter. */
    VkCommandBuffer c = record(&x);
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    assert(c->state == PS5VK_RECORDING && c->xfb_bindings[0].buffer == x.capture &&
           c->xfb_bindings[0].offset == 0 && c->xfb_bindings[0].size == 256);
    vkCmdBeginTransformFeedbackEXT(c, 0, 1, &x.counter, &eight);
    assert(c->state == PS5VK_RECORDING && c->xfb_active && c->operation_count == 1);
    const struct ps5vk_operation *begin = &c->operations[0];
    assert(begin->type == PS5VK_TRANSFORM_FEEDBACK_BEGIN);
    assert(begin->xfb.buffers[0].buffer == x.capture && begin->xfb.buffers[0].size == 256);
    assert(!begin->xfb.buffers[1].buffer);
    assert(begin->xfb.counters[0].buffer == x.counter && begin->xfb.counters[0].offset == 8 &&
           begin->xfb.counters[0].size == 4 && !begin->xfb.counters[1].buffer);
    /* Submission re-validates the immutable record: live ranges, a counter
     * dword, capture ranges only on BEGIN, and the feature still enabled. */
    assert(ps5vk_xfb_operation_valid(x.device, begin));
    struct ps5vk_operation copy = *begin;
    copy.xfb.counters[0].size = 8;
    assert(!ps5vk_xfb_operation_valid(x.device, &copy));
    copy = *begin;
    copy.type = PS5VK_TRANSFORM_FEEDBACK_END;
    assert(!ps5vk_xfb_operation_valid(x.device, &copy));
    copy = *begin;
    copy.xfb.buffers[0].size = 2048;
    assert(!ps5vk_xfb_operation_valid(x.device, &copy));
    /* Capture is fixed while active: no rebind, no pipeline change, no end of
     * the subpass or of the pass, no end of recording. */
    assert(vkEndCommandBuffer(c) == VK_ERROR_UNKNOWN);
    vkCmdEndTransformFeedbackEXT(c, 0, 1, &x.counter, &eight);
    assert(c->state == PS5VK_RECORDING && !c->xfb_active && c->operation_count == 2);
    assert(c->operations[1].type == PS5VK_TRANSFORM_FEEDBACK_END &&
           c->operations[1].xfb.counters[0].buffer == x.counter &&
           !c->operations[1].xfb.buffers[0].buffer);
    leave(c);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);

    c = record(&x);
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, NULL);
    assert(c->xfb_bindings[0].size == 1024); /* WHOLE_SIZE by omission */
    vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL);
    assert(c->state == PS5VK_RECORDING && c->xfb_active);
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    assert(c->state == PS5VK_INVALID);
    c = record(&x);
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, &x.pipeline);
    assert(c->state == PS5VK_INVALID);
    c = record(&x);
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL);
    vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL);
    assert(c->state == PS5VK_INVALID);

    /* Binding rules, each on a fresh recording. */
    const VkDeviceSize odd = 2, past = 1024, too_big = 1020, zero_size = 0;
    struct { uint32_t first, count; VkBuffer buffer; const VkDeviceSize *offset, *size; } bad[] = {
        {0, 1, x.plain, &zero, &size},      /* no transform feedback usage */
        {0, 1, x.capture, &odd, &size},     /* offset not dword aligned */
        {0, 1, x.capture, &past, NULL},     /* offset at the end */
        {0, 1, x.capture, &eight, &too_big},/* range past the end */
        {0, 1, x.capture, &zero, &zero_size},
        {4, 1, x.capture, &zero, &size},    /* firstBinding past the buffers */
        {3, 2, x.capture, &zero, &size},    /* first + count past them */
        {0, 0, x.capture, &zero, &size},
        {0, 1, VK_NULL_HANDLE, &zero, &size},
    };
    for (unsigned n = 0; n < sizeof(bad) / sizeof(bad[0]); ++n) {
        VkBuffer buffers[2] = {bad[n].buffer, bad[n].buffer};
        VkDeviceSize offsets[2] = {*bad[n].offset, *bad[n].offset};
        VkDeviceSize sizes[2] = {bad[n].size ? *bad[n].size : 0, bad[n].size ? *bad[n].size : 0};
        c = record(&x);
        vkCmdBindTransformFeedbackBuffersEXT(c, bad[n].first, bad[n].count, buffers, offsets,
                                             bad[n].size ? sizes : NULL);
        assert(c->state == PS5VK_INVALID);
    }

    /* Begin rules. */
    c = record(&x);
    vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL); /* buffer 0 not bound */
    assert(c->state == PS5VK_INVALID);
    c = record(&x);
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    leave(c);
    vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL); /* outside a render pass */
    assert(c->state == PS5VK_INVALID);
    c = record(&x);
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    x.pipeline.xfb.buffers_mask = 0; /* the bound pipeline does not capture */
    vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL);
    x.pipeline.xfb.buffers_mask = 1;
    assert(c->state == PS5VK_INVALID);
    const VkDeviceSize counter_odd = 2, counter_end = 64;
    const VkDeviceSize *counter_offsets[] = {&counter_odd, &counter_end};
    for (unsigned n = 0; n < 2; ++n) {
        c = record(&x);
        vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
        vkCmdBeginTransformFeedbackEXT(c, 0, 1, &x.counter, counter_offsets[n]);
        assert(c->state == PS5VK_INVALID);
    }
    c = record(&x);
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    vkCmdBeginTransformFeedbackEXT(c, 0, 1, &x.plain, &zero); /* no counter usage */
    assert(c->state == PS5VK_INVALID);
    c = record(&x);
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    vkCmdBeginTransformFeedbackEXT(c, 3, 2, NULL, NULL); /* first + count past four */
    assert(c->state == PS5VK_INVALID);
    /* Counter slot 3 with a null buffer beside it, and default offsets. */
    c = record(&x);
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    VkBuffer two[2] = {VK_NULL_HANDLE, x.counter};
    vkCmdBeginTransformFeedbackEXT(c, 2, 2, two, NULL);
    assert(c->state == PS5VK_RECORDING && !c->operations[0].xfb.counters[2].buffer &&
           c->operations[0].xfb.counters[3].buffer == x.counter &&
           c->operations[0].xfb.counters[3].offset == 0);
    vkCmdEndTransformFeedbackEXT(c, 0, 0, NULL, NULL);
    vkCmdEndTransformFeedbackEXT(c, 0, 0, NULL, NULL); /* not active */
    assert(c->state == PS5VK_INVALID);

    /* Stream queries: two values per query, begun and ended inside one
     * capture session, stream below four, no flags. */
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT, .queryCount = 2};
    VkQueryPool stream_pool;
    assert(vkCreateQueryPool(x.device, &qi, NULL, &stream_pool) == VK_SUCCESS);
    vkResetQueryPool(x.device, stream_pool, 0, 2); /* not enabled: ignored */
    c = record(&x);
    leave(c);
    vkCmdResetQueryPool(c, stream_pool, 0, 2);
    c->render_pass = &x.pass;
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    vkCmdBeginQueryIndexedEXT(c, stream_pool, 0, 0, 1); /* outside a session */
    assert(c->state == PS5VK_INVALID);
    const struct { uint32_t query, index; VkQueryControlFlags flags; } bad_query[] = {
        {0, 4, 0}, {0, 1, VK_QUERY_CONTROL_PRECISE_BIT}, {2, 0, 0}};
    for (unsigned n = 0; n < sizeof(bad_query) / sizeof(bad_query[0]); ++n) {
        c = record(&x);
        leave(c);
        vkCmdResetQueryPool(c, stream_pool, 0, 2);
        c->render_pass = &x.pass;
        vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
        vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL);
        vkCmdBeginQueryIndexedEXT(c, stream_pool, bad_query[n].query, bad_query[n].flags,
                                  bad_query[n].index);
        assert(c->state == PS5VK_INVALID);
    }
    c = record(&x);
    leave(c);
    vkCmdResetQueryPool(c, stream_pool, 0, 2);
    c->render_pass = &x.pass;
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL);
    vkCmdBeginQueryIndexedEXT(c, stream_pool, 1, 0, 2);
    assert(c->state == PS5VK_RECORDING);
    const struct ps5vk_operation *qb = &c->operations[c->operation_count - 1];
    assert(qb->type == PS5VK_QUERY_BEGIN && qb->query_stream == 2 && qb->query_first == 1);
    assert(ps5vk_query_operation_validate(x.device, qb) == VK_SUCCESS);
    vkCmdEndQueryIndexedEXT(c, stream_pool, 1, 1); /* another stream */
    assert(c->state == PS5VK_INVALID);
    c = record(&x);
    leave(c);
    vkCmdResetQueryPool(c, stream_pool, 0, 2);
    c->render_pass = &x.pass;
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL);
    vkCmdBeginQuery(c, stream_pool, 0, 0); /* core command: stream 0 */
    vkCmdEndTransformFeedbackEXT(c, 0, 0, NULL, NULL); /* query still active */
    assert(c->state == PS5VK_INVALID);
    c = record(&x);
    leave(c);
    vkCmdResetQueryPool(c, stream_pool, 0, 2);
    c->render_pass = &x.pass;
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL);
    vkCmdBeginQuery(c, stream_pool, 0, 0);
    vkCmdEndQuery(c, stream_pool, 0);
    vkCmdEndTransformFeedbackEXT(c, 0, 0, NULL, NULL);
    assert(c->state == PS5VK_RECORDING);
    /* Published results read back as (written, needed[, availability]). */
    stream_pool->states[0] = PS5VK_QUERY_UNAVAILABLE;
    assert(ps5vk_query_publish_xfb(x.device, stream_pool, 0, 10, 16) == VK_SUCCESS);
    assert(ps5vk_query_publish_xfb(x.device, stream_pool, 0, 17, 16) != VK_SUCCESS);
    uint64_t result[3] = {0};
    assert(vkGetQueryPoolResults(x.device, stream_pool, 0, 1, sizeof(result), result,
        sizeof(result), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
        VK_SUCCESS);
    assert(result[0] == 10 && result[1] == 16 && result[2] == 1);
    uint32_t small[2] = {0};
    assert(vkGetQueryPoolResults(x.device, stream_pool, 0, 1, sizeof(small), small,
        sizeof(small), 0) == VK_SUCCESS && small[0] == 10 && small[1] == 16);
    vkDestroyQueryPool(x.device, stream_pool, NULL);
    struct VkQueryPool_T occlusion_pool = {.device = x.device,
        .query_type = VK_QUERY_TYPE_OCCLUSION, .query_count = 1};
    c = record(&x);
    vkCmdEndQueryIndexedEXT(c, &occlusion_pool, 0, 1);
    assert(c->state == PS5VK_INVALID);
    /* DrawIndirectByteCount records one draw resolved at the queue head:
     * vertexCount = (counter - counterOffset) / vertexStride. */
    struct ps5vk_operation auto_record = {.type = PS5VK_DRAW_INDIRECT_BYTE_COUNT,
        .indirect_buffer = x.counter, .indirect_offset = 8, .indirect_count = 1,
        .indirect_stride = 32, .byte_count_offset = 32, .instance_count = 2,
        .first_instance = 1};
    const struct ps5vk_operation *auto_draw = &auto_record;
    assert(ps5vk_indirect_graphics_operation(auto_draw->type) &&
           ps5vk_indirect_argument_size(auto_draw->type) == 4);
    void *mapped = NULL;
    assert(vkMapMemory(x.device, x.memory, 2048, 64, 0, &mapped) == VK_SUCCESS);
    const uint32_t counter_words[4] = {0, 0, 128, 0};
    memcpy(mapped, counter_words, sizeof(counter_words));
    struct ps5vk_operation resolved;
    assert(ps5vk_indirect_resolve(x.device, auto_draw, &resolved) == VK_SUCCESS);
    assert(resolved.type == PS5VK_DRAW && resolved.vertex_count == 3 &&
           !resolved.first_vertex && resolved.instance_count == 2 &&
           resolved.first_instance == 1);
    /* A counter at or below the offset draws nothing; a partial vertex is
     * dropped. */
    ((uint32_t *)mapped)[2] = 16;
    assert(ps5vk_indirect_resolve(x.device, auto_draw, &resolved) == VK_SUCCESS &&
           resolved.vertex_count == 0);
    ((uint32_t *)mapped)[2] = 32 + 95;
    assert(ps5vk_indirect_resolve(x.device, auto_draw, &resolved) == VK_SUCCESS &&
           resolved.vertex_count == 2);
    vkUnmapMemory(x.device, x.memory);
    /* Stride 0 or past 2048, a misaligned counter, a counter past the end and
     * a buffer without indirect usage are refused. */
    const struct { VkBuffer buffer; VkDeviceSize offset; uint32_t stride; } bad_auto[] = {
        {x.counter, 0, 0}, {x.counter, 0, 2052}, {x.counter, 2, 16}, {x.counter, 64, 16},
        {x.capture, 0, 16},
    };
    for (unsigned n = 0; n < sizeof(bad_auto) / sizeof(bad_auto[0]); ++n) {
        c = record(&x);
        vkCmdDrawIndirectByteCountEXT(c, 1, 0, bad_auto[n].buffer, bad_auto[n].offset, 0,
                                      bad_auto[n].stride);
        assert(c->state == PS5VK_INVALID);
    }

    /* Barrier scopes: the capture stage and its accesses order work on a
     * device with the feature; an access must name a stage that performs it. */
    c = record(&x);
    leave(c);
    VkBufferMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = x.capture, .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &b, 0, NULL);
    /* One per-buffer record plus the aggregate dependency. */
    assert(c->state == PS5VK_RECORDING && c->operation_count == 2 &&
           c->operations[0].buffer_barrier.srcAccessMask ==
               VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT);
    VkMemoryBarrier counters = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
        .dstAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT |
                         VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &counters, 0, NULL, 0, NULL);
    assert(c->state == PS5VK_RECORDING);
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &b, 0, NULL);
    assert(c->state == PS5VK_INVALID); /* XFB write at the transfer stage */
    fixture_close(&x);

    /* Extension without the feature: every recording command and the capture
     * stage are refused. */
    fixture_open(&x, 0);
    c = record(&x);
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, 1, &x.capture, &zero, &size);
    assert(c->state == PS5VK_INVALID);
    c = record(&x);
    leave(c);
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &b, 0, NULL);
    assert(c->state == PS5VK_INVALID);
    fixture_close(&x);
}

int main(void)
{
    unsupported_platform();
    supported_device();
    recording();
    return 0;
}
