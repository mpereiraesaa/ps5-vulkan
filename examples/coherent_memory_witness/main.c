/* Public-SDK witness for the driver-maintained HOST_COHERENT memory type.
 *
 * Built only with the diagnostic switch that appends the coherent type. The
 * payload never calls vkFlushMappedMemoryRanges or
 * vkInvalidateMappedMemoryRanges on coherent memory, keeps it persistently
 * mapped (as DXVK does) and checks:
 *   C1 host writes reach a compute dispatch, and its writes reach the host;
 *   C2 the same destination lines, already read (cached) by the host, show the
 *      next dispatch's values, not the previous ones (read-after-write across
 *      two submissions);
 *   C3 the host writes one half of every 64-byte line while the dispatch
 *      writes the other half, and both halves survive.
 * A negative control repeats C2 on the non-coherent type WITHOUT flush or
 * invalidate and reports whether stale data was observed; it shows whether
 * the instrument can see a missing cache operation on this console. Every
 * fence wait is bounded. */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "coherent_transform_shader.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { WORDS = 4096, GROUPS = WORDS / 64 };
static const VkDeviceSize BYTES = WORDS * sizeof(uint32_t);
static const uint64_t FENCE_TIMEOUT_NS = UINT64_C(1000000000);

static uint32_t source_word(uint32_t seed, uint32_t i) { return (i * UINT32_C(0x9e3779b9)) ^ seed; }
static uint32_t transform(uint32_t value, uint32_t salt, uint32_t i)
{ return (value * UINT32_C(2654435761)) ^ (salt + i); }
static uint32_t host_marker(uint32_t i) { return UINT32_C(0xc0de0000) | i; }

struct region {
    VkDeviceMemory memory;
    VkBuffer src, dst;
    VkDescriptorSet set;
    uint32_t *map;           /* src at word 0, dst at word WORDS */
};
struct context {
    VkDevice device;
    VkQueue queue;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    VkBool32 pending;
};

static VkResult dispatch(struct context *c, const struct region *r, uint32_t salt,
                         uint32_t split, VkBool32 host_half_during)
{
    VkResult result = vkResetCommandPool(c->device, c->pool, 0);
    if (result != VK_SUCCESS) return result;
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if ((result = vkBeginCommandBuffer(c->command, &begin)) != VK_SUCCESS) return result;
    VkMemoryBarrier to_shader = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(c->command, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &to_shader, 0, NULL, 0, NULL);
    vkCmdBindPipeline(c->command, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeline);
    vkCmdBindDescriptorSets(c->command, VK_PIPELINE_BIND_POINT_COMPUTE, c->layout, 0, 1,
                            &r->set, 0, NULL);
    const uint32_t constants[2] = {salt, split};
    vkCmdPushConstants(c->command, c->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(constants), constants);
    vkCmdDispatch(c->command, GROUPS, 1, 1);
    VkMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(c->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
    if ((result = vkEndCommandBuffer(c->command)) != VK_SUCCESS) return result;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &c->command};
    if ((result = vkQueueSubmit(c->queue, 1, &submit, c->fence)) != VK_SUCCESS) return result;
    c->pending = VK_TRUE;
    /* C3: host writes to the half of each line the dispatch does not write,
     * after the submission and before its completion is observed. */
    if (host_half_during)
        for (uint32_t i = 0; i < WORDS; ++i)
            if ((i & 15u) >= 8u) r->map[WORDS + i] = host_marker(i);
    result = vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, FENCE_TIMEOUT_NS);
    if (result != VK_SUCCESS) return result == VK_TIMEOUT ? VK_ERROR_DEVICE_LOST : result;
    c->pending = VK_FALSE;
    return vkResetFences(c->device, 1, &c->fence);
}

struct tally { uint32_t mismatches, stale, host_half; };
static struct tally compare(const struct region *r, uint32_t seed, uint32_t salt,
                            uint32_t old_seed, uint32_t old_salt, VkBool32 split)
{
    struct tally t = {0};
    for (uint32_t i = 0; i < WORDS; ++i) {
        const uint32_t got = r->map[WORDS + i];
        if (split && (i & 15u) >= 8u) {
            if (got != host_marker(i)) { ++t.mismatches; ++t.host_half; }
            continue;
        }
        if (got != transform(source_word(seed, i), salt, i)) {
            ++t.mismatches;
            if (got == transform(source_word(old_seed, i), old_salt, i)) ++t.stale;
        }
    }
    return t;
}
static void write_source(const struct region *r, uint32_t seed)
{ for (uint32_t i = 0; i < WORDS; ++i) r->map[i] = source_word(seed, i); }

static VkResult make_region(struct context *c, VkDescriptorPool pool, VkDescriptorSetLayout layout,
                            uint32_t type, struct region *r)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BYTES,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult result = vkCreateBuffer(c->device, &info, NULL, &r->src);
    if (result == VK_SUCCESS) result = vkCreateBuffer(c->device, &info, NULL, &r->dst);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(c->device, r->src, &requirements);
    if (!(requirements.memoryTypeBits & (1u << type)) || requirements.size != BYTES)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 2 * BYTES, .memoryTypeIndex = type};
    if ((result = vkAllocateMemory(c->device, &allocation, NULL, &r->memory)) != VK_SUCCESS)
        return result;
    if ((result = vkBindBufferMemory(c->device, r->src, r->memory, 0)) != VK_SUCCESS ||
        (result = vkBindBufferMemory(c->device, r->dst, r->memory, BYTES)) != VK_SUCCESS)
        return result;
    VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &layout};
    if ((result = vkAllocateDescriptorSets(c->device, &set_info, &r->set)) != VK_SUCCESS)
        return result;
    VkDescriptorBufferInfo buffers[2] = {{r->src, 0, BYTES}, {r->dst, 0, BYTES}};
    VkWriteDescriptorSet writes[2];
    for (uint32_t n = 0; n < 2; ++n)
        writes[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->set, .dstBinding = n, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[n]};
    vkUpdateDescriptorSets(c->device, 2, writes, 0, NULL);
    /* Persistent mapping of the whole allocation, as DXVK keeps it. */
    return vkMapMemory(c->device, r->memory, 0, VK_WHOLE_SIZE, 0, (void **)&r->map);
}
static void destroy_region(VkDevice d, struct region *r)
{
    if (r->src) vkDestroyBuffer(d, r->src, NULL);
    if (r->dst) vkDestroyBuffer(d, r->dst, NULL);
    if (r->memory) vkFreeMemory(d, r->memory, NULL);
}

static int run_witness(void)
{
    VkResult result = VK_SUCCESS;
    const char *failed = NULL;
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    failed = #call; goto cleanup; } } while (0)
#define REQUIRE(condition, reason) do { if (!(condition)) { \
    result = VK_ERROR_UNKNOWN; failed = reason; goto cleanup; } } while (0)
    VkInstance instance = VK_NULL_HANDLE;
    struct context c = {0};
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    struct region coherent = {0}, control = {0};
    uint32_t coherent_type = UINT32_MAX;

    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t physical_count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &physical_count, &physical));
    REQUIRE(physical_count == 1 && physical, "one physical device");
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    /* DXVK's selection: the first type holding HOST_VISIBLE|HOST_COHERENT. */
    const VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t n = 0; n < memory.memoryTypeCount && coherent_type == UINT32_MAX; ++n)
        if ((memory.memoryTypes[n].propertyFlags & wanted) == wanted) coherent_type = n;
    ps5log_printf(PS5LOG_MARK,
        "COHERENT_WITNESS_START types=%u type0=%x type1=%x coherent_type=%d words=%u",
        memory.memoryTypeCount, memory.memoryTypes[0].propertyFlags,
        memory.memoryTypeCount > 1 ? memory.memoryTypes[1].propertyFlags : 0u,
        coherent_type == UINT32_MAX ? -1 : (int)coherent_type, WORDS);
    REQUIRE(memory.memoryTypeCount == 2 && coherent_type == 1 &&
            !(memory.memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
            "diagnostic profile: non-coherent type 0, coherent type 1");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    TRY(vkCreateDevice(physical, &device_info, NULL, &c.device));
    vkGetDeviceQueue(c.device, 0, 0, &c.queue);
    REQUIRE(c.queue, "queue exists");

    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(coherent_transform_spirv), .pCode = coherent_transform_spirv};
    TRY(vkCreateShaderModule(c.device, &shader_info, NULL, &module));
    VkDescriptorSetLayoutBinding bindings[2];
    for (uint32_t n = 0; n < 2; ++n)
        bindings[n] = (VkDescriptorSetLayoutBinding){.binding = n,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings};
    TRY(vkCreateDescriptorSetLayout(c.device, &set_info, NULL, &set_layout));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo descriptor_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    TRY(vkCreateDescriptorPool(c.device, &descriptor_pool_info, NULL, &descriptor_pool));
    VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range};
    TRY(vkCreatePipelineLayout(c.device, &layout_info, NULL, &c.layout));
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = c.layout};
    TRY(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                 &c.pipeline));
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    TRY(vkCreateCommandPool(c.device, &pool_info, NULL, &c.pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = c.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    TRY(vkAllocateCommandBuffers(c.device, &command_info, &c.command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(c.device, &fence_info, NULL, &c.fence));
    TRY(make_region(&c, descriptor_pool, set_layout, 1, &coherent));
    TRY(make_region(&c, descriptor_pool, set_layout, 0, &control));

    /* C1: host writes, dispatch, host reads; no flush and no invalidate. */
    write_source(&coherent, 0x11110001u);
    TRY(dispatch(&c, &coherent, 0x5a170001u, 0, VK_FALSE));
    struct tally t1 = compare(&coherent, 0x11110001u, 0x5a170001u, 0, 0, VK_FALSE);
    ps5log_printf(PS5LOG_MARK, "COHERENT_WITNESS_RESULT case=C1 mismatches=%u", t1.mismatches);
    /* C2: the destination lines were just read; the next dispatch rewrites them. */
    write_source(&coherent, 0x22220002u);
    TRY(dispatch(&c, &coherent, 0x5a170002u, 0, VK_FALSE));
    struct tally t2 = compare(&coherent, 0x22220002u, 0x5a170002u,
                              0x11110001u, 0x5a170001u, VK_FALSE);
    ps5log_printf(PS5LOG_MARK, "COHERENT_WITNESS_RESULT case=C2 mismatches=%u stale=%u",
                  t2.mismatches, t2.stale);
    /* C3: host and device each own one half of every 64-byte line. */
    write_source(&coherent, 0x33330003u);
    TRY(dispatch(&c, &coherent, 0x5a170003u, 1, VK_TRUE));
    struct tally t3 = compare(&coherent, 0x33330003u, 0x5a170003u,
                              0x22220002u, 0x5a170002u, VK_TRUE);
    ps5log_printf(PS5LOG_MARK,
        "COHERENT_WITNESS_RESULT case=C3 mismatches=%u stale=%u host_half=%u",
        t3.mismatches, t3.stale, t3.host_half);

    /* Negative control on the non-coherent type: a sane start with explicit
     * flush and invalidate, then the C2 sequence with neither. */
    {
        VkMappedMemoryRange all = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = control.memory, .offset = 0, .size = VK_WHOLE_SIZE};
        write_source(&control, 0x44440004u);
        TRY(vkFlushMappedMemoryRanges(c.device, 1, &all));
        TRY(dispatch(&c, &control, 0x5a170004u, 0, VK_FALSE));
        TRY(vkInvalidateMappedMemoryRanges(c.device, 1, &all));
        struct tally t0 = compare(&control, 0x44440004u, 0x5a170004u, 0, 0, VK_FALSE);
        write_source(&control, 0x55550005u);
        TRY(dispatch(&c, &control, 0x5a170005u, 0, VK_FALSE));
        struct tally tn = compare(&control, 0x55550005u, 0x5a170005u,
                                  0x44440004u, 0x5a170004u, VK_FALSE);
        ps5log_printf(PS5LOG_MARK,
            "COHERENT_WITNESS_CONTROL maintained_mismatches=%u unmaintained_mismatches=%u "
            "stale_host_read=%u verdict=%s",
            t0.mismatches, tn.mismatches, tn.stale,
            t0.mismatches ? "instrument-broken" : tn.mismatches ? "stale-observed" : "no-stale-observed");
        REQUIRE(!t0.mismatches, "control with explicit flush/invalidate");
    }
    REQUIRE(!t1.mismatches && !t2.mismatches && !t3.mismatches,
            "coherent memory without flush or invalidate");
    TRY(vkDeviceWaitIdle(c.device));

cleanup:
    if (c.pending && c.device && vkDeviceWaitIdle(c.device) != VK_SUCCESS) {
        ps5log_printf(PS5LOG_ERR, "COHERENT_WITNESS_FAILURE call=%s result=%d retirement=pending",
                      failed ? failed : "idle", (int)result);
        return 1;
    }
    if (c.device) {
        destroy_region(c.device, &coherent);
        destroy_region(c.device, &control);
        if (c.fence) vkDestroyFence(c.device, c.fence, NULL);
        if (c.command) vkFreeCommandBuffers(c.device, c.pool, 1, &c.command);
        if (c.pool) vkDestroyCommandPool(c.device, c.pool, NULL);
        if (c.pipeline) vkDestroyPipeline(c.device, c.pipeline, NULL);
        if (c.layout) vkDestroyPipelineLayout(c.device, c.layout, NULL);
        if (descriptor_pool) vkDestroyDescriptorPool(c.device, descriptor_pool, NULL);
        if (set_layout) vkDestroyDescriptorSetLayout(c.device, set_layout, NULL);
        if (module) vkDestroyShaderModule(c.device, module, NULL);
        vkDestroyDevice(c.device, NULL);
    }
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK, "COHERENT_WITNESS_RETIRED resources=clean");
    else
        ps5log_printf(PS5LOG_ERR, "COHERENT_WITNESS_FAILURE call=%s result=%d retirement=attempted",
                      failed ? failed : "unknown", (int)result);
    return result == VK_SUCCESS ? 0 : 1;
#undef TRY
#undef REQUIRE
}

int main(void)
{
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t boot = (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
    ps5log_config config;
    const char *loaded = NULL, *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&config);
    if (ps5log_load_config(paths, 1, &config, &loaded)) _exit(1);
    config.udp = 0;
    if (ps5log_init(&config, "PPSA99994", "ps5vk", boot)) _exit(1);
    int failed = run_witness();
    ps5log_close(failed ? "coherent-memory-witness-failed" : "coherent-memory-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}
