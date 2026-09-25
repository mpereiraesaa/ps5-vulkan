/* Public-SDK witness for VK_EXT_robustness2 (DXVK262-T13) on the native queue.
 *
 * The device enables robustBufferAccess (core) together with the extension's
 * robustBufferAccess2 and nullDescriptor, exactly what the pinned DXVK asks
 * for. One compute invocation then reads through ranged and null
 * descriptors and stores through them; the host compares every result word
 * with the robustness2 oracle:
 *   - a 38-byte storage range is bounded at 40 bytes (the reported 4-byte
 *     access alignment): the dword at byte 36 reads its data, the dword at
 *     byte 40 and a dword past it read zero, and stores past it are dropped;
 *   - a 40-byte uniform range reads (x, y, 0, 0) for the vec4 straddling its
 *     end and zero for the next one;
 *   - null storage, uniform, texel-buffer and storage-image descriptors read
 *     zero and drop stores.
 * A second pipeline queries the sizes of the null image and texel buffer
 * (zero expected); a refused query pipeline is reported, not fatal.
 * Every wait is a bounded fence wait. */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include "t13_robustness2_shaders.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    RESULT_WORDS = 64, SOURCE_WORDS = 64,
    STORAGE_RANGE = 38, UNIFORM_RANGE = 40,
    ACCESS_WORDS = 26, QUERY_FIRST = 32, QUERY_WORDS = 3,
    ACCESS_MARKER_WORD = 31, QUERY_MARKER_WORD = 35, NULL_IMAGE_ALPHA_WORD = 25,
    FENCE_SLICE_NS = 300000000, FENCE_SLICES = 10,
};
static const uint32_t guard_word = UINT32_C(0xdeadbeef);
static const uint32_t access_marker = UINT32_C(0xc0de0013);
static const uint32_t query_marker = UINT32_C(0xc0de0014);

/* The robustness2 oracle for the access pipeline, word by word. */
static uint32_t expected_access(uint32_t n)
{
    switch (n) {
    case 0: return 0x1000;        /* ranged.v[0] */
    case 1: return 0x1008;        /* ranged.v[8] */
    case 2: return 0x1009;        /* ranged.v[9]: bytes 36..39, inside 38 rounded to 40 */
    case 3: return 0;             /* ranged.v[10]: byte 40, out of bounds */
    case 4: return 0;             /* ranged.v[63]: inside the buffer, outside the range */
    case 5: return 0x2008;        /* uniform v[2].x: bytes 32..35 */
    case 6: return 0x2009;        /* uniform v[2].y: bytes 36..39 */
    case 13: return 0x2000;       /* uniform v[0].x */
    default: return 0;            /* v[2].zw, v[3], and every null read */
    }
}
/* A null storage image read through a format qualifier without alpha may
 * return alpha 1 (the pinned CTS robustness2 oracle accepts zzzo there). */
static int access_matches(uint32_t n, uint32_t value)
{ return value == expected_access(n) || (n == NULL_IMAGE_ALPHA_WORD && value == 1); }
static uint32_t source_word(uint32_t base, uint32_t n) { return base + n; }

static VkResult wait_bounded(VkDevice device, VkFence fence, uint32_t *slices)
{
    VkResult result = VK_TIMEOUT;
    for (*slices = 0; *slices < FENCE_SLICES && result == VK_TIMEOUT; ++*slices)
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, FENCE_SLICE_NS);
    return result;
}

struct allocation { VkBuffer buffer; VkDeviceMemory memory; VkDeviceSize bytes; };

static VkResult make_buffer(VkDevice device, VkBufferUsageFlags usage, VkDeviceSize size,
                            uint32_t base, struct allocation *out)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult result = vkCreateBuffer(device, &info, NULL, &out->buffer);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirements = {0};
    vkGetBufferMemoryRequirements(device, out->buffer, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = 0};
    result = vkAllocateMemory(device, &allocation, NULL, &out->memory);
    if (result != VK_SUCCESS) return result;
    result = vkBindBufferMemory(device, out->buffer, out->memory, 0);
    if (result != VK_SUCCESS) return result;
    out->bytes = requirements.size;
    uint32_t *words = NULL;
    result = vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, (void **)&words);
    if (result != VK_SUCCESS) return result;
    for (VkDeviceSize j = 0; j < out->bytes / sizeof(uint32_t); ++j)
        words[j] = base ? source_word(base, (uint32_t)j) : guard_word;
    VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = out->memory, .offset = 0, .size = VK_WHOLE_SIZE};
    result = vkFlushMappedMemoryRanges(device, 1, &flush);
    vkUnmapMemory(device, out->memory);
    return result;
}

static VkResult read_words(VkDevice device, const struct allocation *a, uint32_t *out,
                           uint32_t count)
{
    uint32_t *words = NULL;
    VkResult result = vkMapMemory(device, a->memory, 0, VK_WHOLE_SIZE, 0, (void **)&words);
    if (result != VK_SUCCESS) return result;
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = a->memory, .offset = 0, .size = VK_WHOLE_SIZE};
    result = vkInvalidateMappedMemoryRanges(device, 1, &range);
    if (result == VK_SUCCESS) memcpy(out, words, count * sizeof(uint32_t));
    vkUnmapMemory(device, a->memory);
    return result;
}

static void destroy_buffer(VkDevice device, struct allocation *a)
{
    if (a->buffer) vkDestroyBuffer(device, a->buffer, NULL);
    if (a->memory) vkFreeMemory(device, a->memory, NULL);
    *a = (struct allocation){0};
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
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkShaderModule modules[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipelines[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBool32 pending = VK_FALSE;
    struct allocation out = {0}, storage = {0}, uniform = {0};

    const char *instance_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &instance_extension};
    TRY(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t physical_count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance, &physical_count, &physical));
    REQUIRE(physical_count == 1 && physical, "one physical device");

    uint32_t extension_count = 0;
    TRY(vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, NULL));
    VkExtensionProperties extensions[32];
    REQUIRE(extension_count <= 32, "bounded extension list");
    TRY(vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions));
    uint32_t spec = 0;
    for (uint32_t n = 0; n < extension_count; ++n)
        if (!strcmp(extensions[n].extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME))
            spec = extensions[n].specVersion;
    REQUIRE(spec, "VK_EXT_robustness2 enumerated");
    VkPhysicalDeviceRobustness2FeaturesEXT reported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &reported};
    vkGetPhysicalDeviceFeatures2KHR(physical, &features);
    REQUIRE(features.features.robustBufferAccess && reported.robustBufferAccess2 &&
            reported.nullDescriptor && !reported.robustImageAccess2,
            "robustness2 features reported");
    VkPhysicalDeviceRobustness2PropertiesEXT limits = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &limits};
    vkGetPhysicalDeviceProperties2KHR(physical, &properties);
    REQUIRE(limits.robustStorageBufferAccessSizeAlignment == 4 &&
            limits.robustUniformBufferAccessSizeAlignment == 4,
            "robust access size alignments are four bytes");

    const char *device_extension = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceRobustness2FeaturesEXT requested = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .robustBufferAccess2 = VK_TRUE, .nullDescriptor = VK_TRUE};
    VkPhysicalDeviceFeatures2 enabled = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &requested, .features = {.robustBufferAccess = VK_TRUE}};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &device_extension};
    TRY(vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    REQUIRE(queue, "queue exists");
    ps5log_printf(PS5LOG_MARK,
        "T13_ROBUSTNESS2_WITNESS_START spec=%u storage_alignment=%llu uniform_alignment=%llu "
        "storage_range=%u uniform_range=%u",
        spec, (unsigned long long)limits.robustStorageBufferAccessSizeAlignment,
        (unsigned long long)limits.robustUniformBufferAccessSizeAlignment,
        (unsigned)STORAGE_RANGE, (unsigned)UNIFORM_RANGE);

    TRY(make_buffer(device, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    RESULT_WORDS * sizeof(uint32_t), 0, &out));
    TRY(make_buffer(device, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    SOURCE_WORDS * sizeof(uint32_t), 0x1000, &storage));
    TRY(make_buffer(device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    SOURCE_WORDS * sizeof(uint32_t), 0x2000, &uniform));

    const VkShaderModuleCreateInfo shader_info[2] = {
        {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(t13_access_spirv), .pCode = t13_access_spirv},
        {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(t13_query_spirv), .pCode = t13_query_spirv}};
    for (uint32_t n = 0; n < 2; ++n)
        TRY(vkCreateShaderModule(device, &shader_info[n], NULL, &modules[n]));
    const VkDescriptorType types[7] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorSetLayoutBinding bindings[7];
    for (uint32_t n = 0; n < 7; ++n)
        bindings[n] = (VkDescriptorSetLayoutBinding){.binding = n,
            .descriptorType = types[n], .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 7, .pBindings = bindings};
    TRY(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    const VkDescriptorPoolSize pool_sizes[4] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo descriptor_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 4, .pPoolSizes = pool_sizes};
    TRY(vkCreateDescriptorPool(device, &descriptor_pool_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo set_allocation = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    TRY(vkAllocateDescriptorSets(device, &set_allocation, &set));
    const VkDescriptorBufferInfo buffer_infos[5] = {
        {out.buffer, 0, RESULT_WORDS * sizeof(uint32_t)},
        {storage.buffer, 0, STORAGE_RANGE},
        {uniform.buffer, 0, UNIFORM_RANGE},
        {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE},
        {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE}};
    const VkBufferView null_view = VK_NULL_HANDLE;
    const VkDescriptorImageInfo null_image = {VK_NULL_HANDLE, VK_NULL_HANDLE,
                                              VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[7];
    for (uint32_t n = 0; n < 7; ++n) {
        writes[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set, .dstBinding = n, .descriptorCount = 1, .descriptorType = types[n]};
        if (n < 5) writes[n].pBufferInfo = &buffer_infos[n];
        else if (n == 5) writes[n].pTexelBufferView = &null_view;
        else writes[n].pImageInfo = &null_image;
    }
    vkUpdateDescriptorSets(device, 7, writes, 0, NULL);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout};
    TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = modules[0], .pName = "main"},
        .layout = layout};
    TRY(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                 &pipelines[0]));
    pipeline_info.stage.module = modules[1];
    const VkResult query_pipeline = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
        &pipeline_info, NULL, &pipelines[1]);
    if (query_pipeline != VK_SUCCESS) pipelines[1] = VK_NULL_HANDLE;

    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    TRY(vkAllocateCommandBuffers(device, &command_info, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(command, &begin));
    VkMemoryBarrier to_shader = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_UNIFORM_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &to_shader, 0, NULL, 0, NULL);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set,
                            0, NULL);
    for (uint32_t n = 0; n < 2; ++n) {
        if (!pipelines[n]) continue;
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[n]);
        vkCmdDispatch(command, 1, 1, 1);
    }
    VkMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
    TRY(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    TRY(vkQueueSubmit(queue, 1, &submit, fence));
    pending = VK_TRUE;
    uint32_t slices = 0;
    const VkResult waited = wait_bounded(device, fence, &slices);
    ps5log_printf(PS5LOG_MARK,
        "T13_ROBUSTNESS2_WITNESS_FENCE result=%d slices=%u query_pipeline=%d",
        (int)waited, slices, (int)query_pipeline);
    TRY(waited);
    pending = VK_FALSE;

    uint32_t words[RESULT_WORDS], source[SOURCE_WORDS];
    TRY(read_words(device, &out, words, RESULT_WORDS));
    uint32_t access_mismatches = 0, first_mismatch = UINT32_MAX;
    for (uint32_t n = 0; n < ACCESS_WORDS; ++n)
        if (!access_matches(n, words[n])) {
            ++access_mismatches;
            if (first_mismatch == UINT32_MAX) first_mismatch = n;
        }
    for (uint32_t n = 0; n < ACCESS_WORDS; ++n)
        ps5log_printf(PS5LOG_MARK, "T13_ROBUSTNESS2_WITNESS_WORD index=%u value=%08x expected=%08x",
                      n, words[n], expected_access(n));
    uint32_t guard_mismatches = 0;
    for (uint32_t n = ACCESS_WORDS; n < RESULT_WORDS; ++n) {
        const int query = n >= QUERY_FIRST && n < QUERY_FIRST + QUERY_WORDS;
        if (n == ACCESS_MARKER_WORD || n == QUERY_MARKER_WORD || query) continue;
        if (words[n] != guard_word) ++guard_mismatches;
    }
    TRY(read_words(device, &storage, source, SOURCE_WORDS));
    uint32_t store_mismatches = 0;
    for (uint32_t n = 0; n < SOURCE_WORDS; ++n) {
        const uint32_t expected = n == 1 ? UINT32_C(0x5a5a0001) : source_word(0x1000, n);
        if (source[n] != expected) ++store_mismatches;
    }
    ps5log_printf(PS5LOG_MARK,
        "T13_ROBUSTNESS2_WITNESS_ACCESS marker=%08x words=%u mismatches=%u first=%d "
        "guard_mismatches=%u store_mismatches=%u in_range_store=%08x dropped_store_12=%08x "
        "dropped_store_60=%08x",
        words[ACCESS_MARKER_WORD], (unsigned)ACCESS_WORDS, access_mismatches,
        first_mismatch == UINT32_MAX ? -1 : (int)first_mismatch, guard_mismatches,
        store_mismatches, source[1], source[12], source[60]);
    if (pipelines[1])
        ps5log_printf(PS5LOG_MARK,
            "T13_ROBUSTNESS2_WITNESS_QUERY pipeline=created marker=%08x image_width=%u "
            "image_height=%u texel_size=%u",
            words[QUERY_MARKER_WORD], words[QUERY_FIRST], words[QUERY_FIRST + 1],
            words[QUERY_FIRST + 2]);
    else
        ps5log_printf(PS5LOG_MARK, "T13_ROBUSTNESS2_WITNESS_QUERY pipeline=refused result=%d",
                      (int)query_pipeline);
    REQUIRE(words[ACCESS_MARKER_WORD] == access_marker && !access_mismatches &&
            !guard_mismatches && !store_mismatches, "robustness2 access oracle");
    REQUIRE(!pipelines[1] || (words[QUERY_MARKER_WORD] == query_marker &&
            !words[QUERY_FIRST] && !words[QUERY_FIRST + 1] && !words[QUERY_FIRST + 2]),
            "null size queries return zero");

cleanup:
    if (pending) {
        /* The fence never signalled within the bound: leave every object
         * alive rather than free memory the GPU may still touch. */
        ps5log_printf(PS5LOG_ERR,
            "T13_ROBUSTNESS2_WITNESS_FAILURE call=%s result=%d retirement=pending",
            failed ? failed : "fence", (int)result);
        return 1;
    }
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command) vkFreeCommandBuffers(device, pool, 1, &command);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    for (uint32_t n = 0; n < 2; ++n) {
        if (pipelines[n]) vkDestroyPipeline(device, pipelines[n], NULL);
        if (modules[n]) vkDestroyShaderModule(device, modules[n], NULL);
    }
    if (layout) vkDestroyPipelineLayout(device, layout, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (device) {
        destroy_buffer(device, &out);
        destroy_buffer(device, &storage);
        destroy_buffer(device, &uniform);
        vkDestroyDevice(device, NULL);
    }
    if (instance) vkDestroyInstance(instance, NULL);
    if (result == VK_SUCCESS)
        ps5log_printf(PS5LOG_MARK, "T13_ROBUSTNESS2_WITNESS_RETIRED resources=clean");
    else
        ps5log_printf(PS5LOG_ERR,
            "T13_ROBUSTNESS2_WITNESS_FAILURE call=%s result=%d retirement=attempted",
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
    ps5log_close(failed ? "t13-robustness2-witness-failed" : "t13-robustness2-witness-end");
    for (;;) sleep(1); /* Wait for the runner's Close Game after retirement. */
}
