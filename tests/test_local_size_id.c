#include "vk_internal.h"
#include "vk_pipeline.h"
#include "compilation_cache.h"
#include "ps5vk_compiler.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz % 4) != 0) { fclose(f); return NULL; }
    uint32_t *buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = malloc(size); *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache_flush(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, cache_flush, cache_flush};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }

VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
                                 .max_allocation = 65536, .queue_flags = VK_QUEUE_COMPUTE_BIT};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU",
        .heap_size = 65536,
        .allocation_granularity = 1,
        .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

/* LocalSizeId (maintenance4): the pinned minimal compute module rewritten in
 * place from OpExecutionMode LocalSize 64 1 1 to OpExecutionModeId
 * LocalSizeId naming its own 32-bit OpConstants 64 and 1 (same length), with
 * the header raised to SPIR-V 1.2, where OpExecutionModeId exists. */
static int rewrite_local_size_id(uint32_t *w, size_t count, uint32_t x_id, uint32_t one_id)
{
    for (size_t i = 5; i < count; i += w[i] >> 16) {
        if ((w[i] & 0xffff) == 16 && w[i] >> 16 == 6 && w[i + 2] == 17) {
            if (w[i + 3] != 64 || w[i + 4] != 1 || w[i + 5] != 1) return 0;
            w[i] = 6u << 16 | 331u; w[i + 2] = 38;
            w[i + 3] = x_id; w[i + 4] = one_id; w[i + 5] = one_id;
            w[1] = 0x00010200u;
            return 1;
        }
    }
    return 0;
}
static uint32_t constant_id(const uint32_t *w, size_t count, uint32_t value)
{
    uint32_t type = 0;
    for (size_t i = 5; i < count; i += w[i] >> 16)
        if ((w[i] & 0xffff) == 21 && w[i] >> 16 == 4 && w[i + 2] == 32 && w[i + 3] == 0)
            type = w[i + 1];
    for (size_t i = 5; i < count; i += w[i] >> 16)
        if ((w[i] & 0xffff) == 43 && w[i] >> 16 == 4 && w[i + 1] == type && w[i + 3] == value)
            return w[i + 2];
    return 0;
}
static VkResult build_specialized(VkDevice device, VkPipelineLayout layout, const uint32_t *words,
                      size_t bytes, const VkSpecializationInfo *specialization, VkPipeline *out)
{
    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes, .pCode = words};
    VkShaderModule module;
    assert(vkCreateShaderModule(device, &smci, NULL, &module) == VK_SUCCESS);
    VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = layout, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main",
            .pSpecializationInfo = specialization}};
    *out = VK_NULL_HANDLE;
    VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, out);
    vkDestroyShaderModule(device, module, NULL);
    return result;
}
static VkResult build(VkDevice device, VkPipelineLayout layout, const uint32_t *words,
                      size_t bytes, VkPipeline *out)
{
    return build_specialized(device, layout, words, bytes, NULL, out);
}

/* Turn the X dimension into a decorated OpSpecConstant without changing the
 * module's ID space. Annotations precede types, as required by SPIR-V. */
static uint32_t *specialize_dimension(const uint32_t *words, size_t count, uint32_t id)
{
    size_t types = 5;
    while (types < count && (words[types] & 0xffff) != 19)
        types += words[types] >> 16;
    assert(types < count);
    uint32_t *out = malloc((count + 4) * sizeof(*out));
    assert(out);
    memcpy(out, words, types * sizeof(*out));
    uint32_t decoration[4] = {4u << 16 | 71u, id, 1u, 7u};
    memcpy(out + types, decoration, sizeof(decoration));
    memcpy(out + types + 4, words + types, (count - types) * sizeof(*out));
    unsigned found = 0;
    for (size_t i = 5; i < count + 4; i += out[i] >> 16)
        if ((out[i] & 0xffff) == 43 && out[i] >> 16 == 4 && out[i + 2] == id) {
            out[i] = 4u << 16 | 50u;
            ++found;
        }
    assert(found == 1);
    return out;
}

int main(void)
{
    size_t bytes = 0;
    uint32_t *literal = read_file("build/test-shaders/minimal.spv", &bytes);
    assert(literal);
    const size_t count = bytes / 4;
    uint32_t *by_id = malloc(bytes), *spec = malloc(bytes);
    assert(by_id && spec);
    memcpy(by_id, literal, bytes);
    const uint32_t id64 = constant_id(literal, count, 64), id1 = constant_id(literal, count, 1);
    assert(id64 && id1 && rewrite_local_size_id(by_id, count, id64, id1));
    /* An operand that is not an OpConstant (here the uint type id) is refused. */
    memcpy(spec, literal, bytes);
    uint32_t type = 0;
    for (size_t i = 5; i < count; i += literal[i] >> 16)
        if ((literal[i] & 0xffff) == 21) { type = literal[i + 1]; break; }
    assert(type && rewrite_local_size_id(spec, count, type, id1));

    VkInstance instance;
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t physical_count = 1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &physical_count, &physical) == VK_SUCCESS);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice device;
    assert(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);
    device->compiler.compile = ps5vk_compiler_adapter_compile;
    ps5vk_device_enable_runtime_compiler(device);
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout sl;
    assert(vkCreateDescriptorSetLayout(device, &slci, NULL, &sl) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &sl};
    VkPipelineLayout layout;
    assert(vkCreatePipelineLayout(device, &plci, NULL, &layout) == VK_SUCCESS);

    VkPipeline reference, pipeline;
    assert(build(device, layout, literal, bytes, &reference) == VK_SUCCESS);
    assert(reference->program.local_size[0] == 64 && reference->program.local_size[1] == 1 &&
           reference->program.local_size[2] == 1);
    /* Without maintenance4 the LocalSizeId form is refused. */
    assert(build(device, layout, by_id, bytes, &pipeline) != VK_SUCCESS && !pipeline);
    /* Set the feature directly to isolate shader admission from device
     * negotiation. The compiled workgroup/code equal the literal form. */
    device->enabled_features_t09 |= PS5VK_T09_FEATURE_MAINTENANCE4;
    assert(build(device, layout, by_id, bytes, &pipeline) == VK_SUCCESS);
    assert(!memcmp(pipeline->program.local_size, reference->program.local_size,
                   sizeof(reference->program.local_size)));
    assert(pipeline->program.code_words == reference->program.code_words &&
           !memcmp(pipeline->program.code, reference->program.code,
                   reference->program.code_words * 4));
    VkPipeline refused;
    assert(build(device, layout, spec, bytes, &refused) != VK_SUCCESS && !refused);

    uint32_t *specialized = specialize_dimension(by_id, count, id64);
    VkPipeline default_size, size32, size16, warm32;
    assert(build(device, layout, specialized, bytes + 16, &default_size) == VK_SUCCESS);
    assert(default_size->program.local_size[0] == 64);
    uint32_t x = 32;
    VkSpecializationMapEntry map = {.constantID = 7, .offset = 0, .size = sizeof(x)};
    VkSpecializationInfo specialization = {.mapEntryCount = 1, .pMapEntries = &map,
        .dataSize = sizeof(x), .pData = &x};
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &size32) == VK_SUCCESS);
    assert(size32->program.local_size[0] == 32 && size32->program.local_size[1] == 1 &&
           size32->program.local_size[2] == 1);
    x = 16;
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &size16) == VK_SUCCESS);
    assert(size16->program.local_size[0] == 16);
    x = 32;
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &warm32) == VK_SUCCESS);
    assert(warm32->program.local_size[0] == 32);
    map.size = 2;
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &refused) != VK_SUCCESS && !refused);
    map.size = sizeof(x); map.offset = sizeof(x);
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &refused) != VK_SUCCESS && !refused);
    map.offset = 0;
    VkSpecializationMapEntry duplicates[2] = {map, map};
    specialization.mapEntryCount = 2; specialization.pMapEntries = duplicates;
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &refused) != VK_SUCCESS && !refused);
    specialization.mapEntryCount = 1; specialization.pMapEntries = NULL;
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &refused) != VK_SUCCESS && !refused);
    vkDestroyPipeline(device, warm32, NULL);
    vkDestroyPipeline(device, size16, NULL);
    vkDestroyPipeline(device, size32, NULL);
    vkDestroyPipeline(device, default_size, NULL);
    free(specialized);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipeline(device, reference, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, sl, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    free(literal); free(by_id); free(spec);
    puts("LocalSizeId: constants and direct specialization workgroup dimensions under maintenance4 "
         "(host compiler, no GPU evidence)");
    return 0;
}
