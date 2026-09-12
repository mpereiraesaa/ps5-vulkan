#include "vk_internal.h"
#include "vk_pipeline.h"
#include "compilation_cache.h"
#include "ps5vk_compiler.h"
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
    p->properties.apiVersion = VK_API_VERSION_1_0;
    strcpy(p->properties.deviceName, "host mock, not a GPU");
    p->properties.limits.nonCoherentAtomSize = 64;
    p->properties.limits.minStorageBufferOffsetAlignment = 256;
    p->memory_properties.memoryTypeCount = p->memory_properties.memoryHeapCount = 1;
    p->memory_properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    p->memory_properties.memoryHeaps[0].size = 65536;
    return VK_SUCCESS;
}

int main(void)
{
    size_t spv_bytes = 0;
    uint32_t *spv = read_file("build/compute/Shader_0xAB87313840E48C25.spv", &spv_bytes);
    if (!spv) spv = read_file("../../build/compute/Shader_0xAB87313840E48C25.spv", &spv_bytes);
    assert(spv != NULL);

    VkInstance instance;
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);

    uint32_t count = 1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci
    };
    VkDevice device;
    assert(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);

    /* Hook runtime compiler adapter into device */
    device->compiler.compile = ps5vk_compiler_adapter_compile;
    ps5vk_device_enable_runtime_compiler(device);

    /* Descriptor layout: binding 0 and binding 1 */
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutCreateInfo slci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings
    };
    VkDescriptorSetLayout sl;
    assert(vkCreateDescriptorSetLayout(device, &slci, NULL, &sl) == VK_SUCCESS);

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &sl
    };
    VkPipelineLayout layout;
    assert(vkCreatePipelineLayout(device, &plci, NULL, &layout) == VK_SUCCESS);

    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spv_bytes, .pCode = spv
    };
    VkShaderModule module;
    assert(vkCreateShaderModule(device, &smci, NULL, &module) == VK_SUCCESS);

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = layout,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main"
        }
    };

    /* 1. Cold compile: pipeline 1 invokes compiler */
    VkPipeline pipeline1;
    assert(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline1) == VK_SUCCESS);

    struct ps5vk_cache_stats stats;
    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &stats);
    assert(stats.compiles == 1);
    assert(stats.misses == 1);
    assert(stats.hits == 0);
    assert(stats.current_entries == 1);

    /* Verify pipeline 1 execution metadata */
    assert(pipeline1->program.gfx == 1013);
    assert(pipeline1->program.wave_size == 32);
    assert(pipeline1->program.user_sgprs == 3);
    assert(pipeline1->program.wgp_mode == 1);
    assert(pipeline1->cache_entry != NULL);

    /* 2. Warm cache: pipeline 2 reuses cached result without compiler invocation */
    VkPipeline pipeline2;
    assert(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline2) == VK_SUCCESS);

    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &stats);
    assert(stats.compiles == 1); /* Compiler NOT invoked again */
    assert(stats.hits == 1);     /* Cache hit! */
    assert(stats.current_entries == 1);

    /* 3. Destroy pipeline 1 */
    vkDestroyPipeline(device, pipeline1, NULL);
    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &stats);
    assert(stats.current_entries == 1); /* Still held by pipeline 2 */

    /* 4. Destroy pipeline 2 */
    vkDestroyPipeline(device, pipeline2, NULL);
    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &stats);
    assert(stats.current_entries == 1); /* Entry stays in cache with refcount 0 */

    /* 5. Create pipeline 3: warm cache hit again */
    VkPipeline pipeline3;
    assert(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline3) == VK_SUCCESS);
    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &stats);
    assert(stats.compiles == 1);
    assert(stats.hits == 2);

    /* Teardown */
    vkDestroyPipeline(device, pipeline3, NULL);
    vkDestroyShaderModule(device, module, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, sl, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    free(spv);

    puts("Runtime pipeline compilation and cache lifecycle: pass (cold compile, warm hit, refcounting)");
    return 0;
}
