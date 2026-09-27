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

static unsigned compile_calls;
static VkResult counted_compile(void *context, const uint32_t *spirv, size_t words,
    const char *entry, VkPipelineLayout layout, const VkSpecializationInfo *specialization,
    uint32_t features, struct ps5vk_compiled_program *program, uint32_t **code)
{
    ++compile_calls;
    return ps5vk_compiler_adapter_compile(context, spirv, words, entry, layout,
                                         specialization, features, program, code);
}

static void cache_control(VkDevice d, const VkComputePipelineCreateInfo *base)
{
    struct ps5vk_compilation_cache *saved = d->pipeline_cache;
    d->pipeline_cache = ps5vk_compilation_cache_create(8, 1024 * 1024);
    assert(d->pipeline_cache);
    d->compiler.compile = counted_compile;
    VkComputePipelineCreateInfo infos[3] = {*base, *base, *base};
    VkPipeline out[3];
    infos[0].flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    assert(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, infos, NULL, out) == VK_ERROR_FEATURE_NOT_PRESENT && !out[0]);
    d->enabled_features_t09 |= PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL;
    assert(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, infos, NULL, out) == VK_PIPELINE_COMPILE_REQUIRED && !out[0]);
    assert(!compile_calls);
    infos[0].flags |= VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT;
    memset(out, 0xff, sizeof(out));
    assert(vkCreateComputePipelines(d, VK_NULL_HANDLE, 3, infos, NULL, out) == VK_PIPELINE_COMPILE_REQUIRED);
    assert(!out[0] && !out[1] && !out[2] && !compile_calls);
    infos[0].flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    infos[2].flags = infos[0].flags;
    assert(vkCreateComputePipelines(d, VK_NULL_HANDLE, 3, infos, NULL, out) == VK_PIPELINE_COMPILE_REQUIRED);
    assert(!out[0] && out[1] && out[2] && compile_calls == 1);
    vkDestroyPipeline(d, out[1], NULL); vkDestroyPipeline(d, out[2], NULL);
    assert(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, infos, NULL, out) == VK_SUCCESS && out[0]);
    assert(compile_calls == 1);
    vkDestroyPipeline(d, out[0], NULL);
    infos[1].flags = VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT;
    infos[1].stage.module = VK_NULL_HANDLE;
    assert(vkCreateComputePipelines(d, VK_NULL_HANDLE, 3, infos, NULL, out) < 0);
    assert(out[0] && !out[1] && !out[2] && compile_calls == 1);
    vkDestroyPipeline(d, out[0], NULL);
    /* Derivative hints do not require cache-control feature opt-in and do
     * not create a different executable-cache identity. */
    d->enabled_features_t09 &= ~PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL;
    infos[0]=*base; infos[1]=*base; infos[2]=*base;
    infos[0].flags=VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
    infos[1].flags=VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
    infos[1].basePipelineIndex=0;
    infos[2].flags=VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    infos[2].basePipelineIndex=1;
    assert(vkCreateComputePipelines(d,0,3,infos,NULL,out)==VK_SUCCESS);
    assert(out[0] && out[1] && out[2] && compile_calls==1);
    VkPipeline parent=out[0], child=out[1];
    assert(parent->allow_derivatives && child->allow_derivatives && !out[2]->allow_derivatives);
    vkDestroyPipeline(d,out[2],NULL);
    infos[2].basePipelineHandle=parent; infos[2].basePipelineIndex=-1;
    d->enabled_features_t09 |= PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL;
    infos[2].flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    VkPipeline derivative;
    assert(vkCreateComputePipelines(d,0,1,&infos[2],NULL,&derivative)==VK_SUCCESS && derivative);
    assert(compile_calls==1);
    parent->allow_derivatives=VK_FALSE;
    assert(vkCreateComputePipelines(d,0,1,&infos[2],NULL,out)<0 && !out[0]);
    parent->allow_derivatives=VK_TRUE;
    parent->device=VK_NULL_HANDLE;
    assert(vkCreateComputePipelines(d,0,1,&infos[2],NULL,out)<0 && !out[0]);
    parent->device=d; parent->graphics=VK_TRUE;
    assert(vkCreateComputePipelines(d,0,1,&infos[2],NULL,out)<0 && !out[0]);
    parent->graphics=VK_FALSE;
    infos[2].basePipelineIndex=0;
    assert(vkCreateComputePipelines(d,0,1,&infos[2],NULL,out)<0 && !out[0]);
    infos[2].basePipelineHandle=VK_NULL_HANDLE;
    for(int index=-2;index<=1;++index) {
        infos[2].basePipelineIndex=index;
        assert(vkCreateComputePipelines(d,0,1,&infos[2],NULL,out)<0 && !out[0]);
    }
    infos[0].flags=0;
    assert(vkCreateComputePipelines(d,0,2,infos,NULL,out)<0 && out[0] && !out[1]);
    vkDestroyPipeline(d,out[0],NULL);
    /* A derivative with different code must use its own shader, not inherit
     * the base executable or turn a cold miss into a false cache hit. */
    size_t xor_bytes=0;
    uint32_t *xor_words=read_file("build/test-shaders/xor.spv",&xor_bytes);
    assert(xor_words);
    VkShaderModuleCreateInfo xor_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=xor_bytes,.pCode=xor_words};
    VkShaderModule xor_module;
    assert(vkCreateShaderModule(d,&xor_info,NULL,&xor_module)==VK_SUCCESS);
    free(xor_words);
    infos[2]=*base; infos[2].stage.module=xor_module;
    infos[2].flags=VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    infos[2].basePipelineHandle=parent;infos[2].basePipelineIndex=-1;
    VkPipeline different;
    assert(vkCreateComputePipelines(d,0,1,&infos[2],NULL,&different)==VK_PIPELINE_COMPILE_REQUIRED && !different);
    assert(compile_calls==1);
    infos[2].flags=VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    assert(vkCreateComputePipelines(d,0,1,&infos[2],NULL,&different)==VK_SUCCESS && different);
    assert(compile_calls==2);
    assert(different->program.code_words!=parent->program.code_words ||
        memcmp(different->program.code,parent->program.code,parent->program.code_words*4));
    vkDestroyShaderModule(d,xor_module,NULL);
    vkDestroyPipeline(d,different,NULL);
    vkDestroyPipeline(d,parent,NULL);
    /* Both children retain their executable after the base is destroyed. */
    assert(child->program.code_words && derivative->program.code_words);
    assert(!memcmp(child->program.code,derivative->program.code,child->program.code_words*4));
    vkDestroyPipeline(d,child,NULL); vkDestroyPipeline(d,derivative,NULL);
    ps5vk_compilation_cache_destroy(d->pipeline_cache);
    d->pipeline_cache = saved;
    d->compiler.compile = ps5vk_compiler_adapter_compile;
    d->enabled_features_t09 &= ~PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL;
}

/* Run the actual public-SDK witness helper with the real host compiler and
 * a synthetic queue. The queue checks the recording and injects controlled
 * data; this proves the oracle and object contracts, not shader execution. */
#include "vk_queue.h"
#include "../examples/dxvk_render_witness/cache_compute.h"
static unsigned witness_fault, witness_launches;
static uint32_t witness_code_hash[2];
static VkResult witness_compile(void *context, const uint32_t *spirv, size_t words,
    const char *entry, VkPipelineLayout layout, const VkSpecializationInfo *specialization,
    uint32_t features, struct ps5vk_compiled_program *program, uint32_t **code)
{
    VkResult r = counted_compile(context, spirv, words, entry, layout, specialization,
        features, program, code);
    assert(r == VK_SUCCESS && compile_calls <= 2);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < program->code_words; ++i) hash = (hash ^ (*code)[i]) * 16777619u;
    witness_code_hash[compile_calls - 1] = hash;
    return r;
}
struct witness_job { uint64_t serial; unsigned char *data; };
static VkResult witness_prepare(VkDevice device, const struct ps5vk_submission *s, void **out)
{
    assert(s->count == 1 && s->buffers[0]->operation_count == 3);
    const struct ps5vk_operation *ops = s->buffers[0]->operations;
    assert(ops[0].type == PS5VK_BARRIER && ops[1].type == PS5VK_DISPATCH &&
        ops[2].type == PS5VK_BARRIER);
    assert(ops[0].src_stage == VK_PIPELINE_STAGE_HOST_BIT &&
        ops[0].dst_stage == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT &&
        ops[0].src_access == VK_ACCESS_HOST_WRITE_BIT &&
        ops[0].dst_access == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT));
    assert(ops[2].src_stage == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT &&
        ops[2].dst_stage == VK_PIPELINE_STAGE_HOST_BIT &&
        ops[2].src_access == VK_ACCESS_SHADER_WRITE_BIT &&
        ops[2].dst_access == VK_ACCESS_HOST_READ_BIT);
    assert(ops[1].groups[0] == 16 && ops[1].groups[1] == 1 && ops[1].groups[2] == 1);
    assert(ops[1].pipeline->program.code_words && compile_calls == 2 &&
        witness_code_hash[0] != witness_code_hash[1]);
    uint32_t executable_hash = 2166136261u;
    for (size_t i = 0; i < ops[1].pipeline->program.code_words; ++i)
        executable_hash = (executable_hash ^ ops[1].pipeline->program.code[i]) * 16777619u;
    assert(executable_hash == witness_code_hash[1]);
    VkDescriptorSet set = ops[1].sets[0];
    assert(set && set->buffers[0].buffer == set->buffers[1].buffer &&
        set->buffers[0].offset == CACHE_INPUT && set->buffers[1].offset == CACHE_OUTPUT &&
        set->buffers[0].range == CACHE_WORDS * 4 && set->buffers[1].range == CACHE_WORDS * 4);
    struct witness_job *job = calloc(1, sizeof(*job));
    assert(job); job->serial = s->serial;
    void *address; VkDeviceSize size;
    assert(ps5vk_buffer_span(device, set->buffers[0].buffer, 0, VK_WHOLE_SIZE,
        &address, &size) == VK_SUCCESS && size >= CACHE_BYTES);
    job->data = address; *out = job;
    return VK_SUCCESS;
}
static VkResult witness_launch(VkDevice d, void *data)
{
    (void)d; struct witness_job *job = data; ++witness_launches;
    for (uint32_t i = 0; i < CACHE_WORDS; ++i) {
        uint32_t input, value;
        memcpy(&input, job->data + CACHE_INPUT + i * 4, 4);
        value = input * (witness_fault == 1 ? 3u : 5u) + (i ^ 0x13579bdfu);
        memcpy(job->data + CACHE_OUTPUT + i * 4, &value, 4);
    }
    if (witness_fault == 2) job->data[CACHE_OUTPUT - 1] ^= 1;
    if (witness_fault == 3) job->data[CACHE_INPUT] ^= 1;
    return VK_SUCCESS;
}
static VkResult witness_poll(VkDevice d, void *data, uint64_t *serial)
{ (void)d; *serial = ((struct witness_job *)data)->serial; return VK_SUCCESS; }
static void witness_release(VkDevice d, void *data) { (void)d; free(data); }
static uint64_t witness_clock(void *context) { (void)context; return 0; }
static void witness_pause(void *context, uint64_t timeout)
{ (void)context; (void)timeout; assert(!"synthetic queue should complete on first poll"); }
static void check_compute_execution_witness(VkDevice d)
{
    size_t bytes;
    uint32_t *words = read_file("build/test-shaders/cache_witness.spv", &bytes);
    assert(words);
    struct ps5vk_compilation_cache *saved_cache = d->pipeline_cache;
    struct ps5vk_queue_backend saved_backend = d->submit_backend;
    struct ps5vk_progress saved_progress = d->progress;
    d->progress = (struct ps5vk_progress){NULL, ps5vk_queue_poll, witness_clock, witness_pause};
    unsigned pipelines = d->pipeline_objects, descriptors = d->descriptor_objects;
    d->enabled_features_t09 |= PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL;
    d->compiler.compile = witness_compile;
    d->submit_backend = (struct ps5vk_queue_backend){witness_prepare, witness_launch,
        witness_poll, witness_release};
    VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);
    for (witness_fault = 0; witness_fault < 4; ++witness_fault) {
        d->pipeline_cache = ps5vk_compilation_cache_create(8, 1024 * 1024);
        assert(d->pipeline_cache); compile_calls = witness_launches = 0;
        VkBool32 pending = VK_TRUE;
        struct cache_compute_result result;
        VkResult rc = cache_compute_witness(d, queue, words, bytes, &pending, &result);
        if (rc != (witness_fault ? VK_ERROR_UNKNOWN : VK_SUCCESS))
            fprintf(stderr, "compute witness fault=%u rc=%d step=%s\n", witness_fault, rc, result.step);
        assert(rc == (witness_fault ? VK_ERROR_UNKNOWN : VK_SUCCESS));
        assert(!pending && compile_calls == 2 && witness_launches == 1);
        assert(result.mismatches == (witness_fault == 1 ? CACHE_WORDS : 0));
        assert(result.guards == (witness_fault == 2) && result.inputs == (witness_fault == 3));
        assert(d->pipeline_objects == pipelines && d->descriptor_objects == descriptors &&
            !d->buffers && !d->memories && !d->command_pools && !d->fences && !d->pipeline_caches &&
            !d->lifetime_errors);
        ps5vk_compilation_cache_destroy(d->pipeline_cache);
    }
    d->pipeline_cache = saved_cache; d->submit_backend = saved_backend;
    d->progress = saved_progress;
    d->enabled_features_t09 &= ~PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL;
    d->compiler.compile = ps5vk_compiler_adapter_compile;
    free(words);
    puts("SDK compute cache witness: pass (real compiler, synthetic queue, three oracle faults; no GPU)");
}

int main(void)
{
    size_t spv_bytes = 0;
    uint32_t *spv = read_file("build/test-shaders/minimal.spv", &spv_bytes);
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

    cache_control(device, &cpci);

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

    /* 5b. A live Vulkan pipeline cache is accepted and changes nothing about
     * compilation: this slice stores no portable records, so the internal
     * compiled-code cache remains the only source of executable code. */
    VkPipelineCacheCreateInfo cache_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache vulkan_cache;
    assert(vkCreatePipelineCache(device, &cache_info, NULL, &vulkan_cache) == VK_SUCCESS);
    VkPipeline pipeline_with_cache;
    assert(vkCreateComputePipelines(device, vulkan_cache, 1, &cpci, NULL, &pipeline_with_cache) == VK_SUCCESS);
    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &stats);
    assert(stats.compiles == 1);   /* still no recompilation */
    assert(stats.hits == 3);       /* the internal cache answered, not the blob */
    assert(pipeline_with_cache->program.gfx == 1013);
    vkDestroyPipeline(device, pipeline_with_cache, NULL);
    vkDestroyPipelineCache(device, vulkan_cache, NULL);

    /* Narrow storage is rejected before compiler/cache access unless the exact
     * device feature is enabled. The feature mask also partitions cache keys. */
    size_t narrow_bytes = 0;
    uint32_t *narrow_spv = read_file("build/test-shaders/storage8.spv", &narrow_bytes);
    assert(narrow_spv);
    smci.codeSize = narrow_bytes; smci.pCode = narrow_spv;
    VkShaderModule narrow_module;
    assert(vkCreateShaderModule(device, &smci, NULL, &narrow_module) == VK_SUCCESS);
    cpci.stage.module = narrow_module;
    VkPipeline narrow_pipeline = VK_NULL_HANDLE;
    assert(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                    &narrow_pipeline) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(!narrow_pipeline);
    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &stats);
    assert(stats.compiles == 1 && stats.current_entries == 1);

    device->enabled_features = PS5VK_FEATURE_STORAGE_BUFFER_8BIT;
    assert(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                    &narrow_pipeline) == VK_SUCCESS);
    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &stats);
    assert(stats.compiles == 2 && stats.current_entries == 2);
    vkDestroyPipeline(device, narrow_pipeline, NULL);

    device->enabled_features = PS5VK_FEATURE_STORAGE_BUFFER_16BIT;
    assert(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                    &narrow_pipeline) == VK_ERROR_FEATURE_NOT_PRESENT);
    vkDestroyShaderModule(device, narrow_module, NULL);
    free(narrow_spv);

    narrow_spv = read_file("build/test-shaders/storage16.spv", &narrow_bytes);
    assert(narrow_spv);
    smci.codeSize = narrow_bytes; smci.pCode = narrow_spv;
    assert(vkCreateShaderModule(device, &smci, NULL, &narrow_module) == VK_SUCCESS);
    cpci.stage.module = narrow_module;
    assert(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                    &narrow_pipeline) == VK_SUCCESS);
    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &stats);
    assert(stats.compiles == 3 && stats.current_entries == 3);
    vkDestroyPipeline(device, narrow_pipeline, NULL);
    vkDestroyShaderModule(device, narrow_module, NULL);
    free(narrow_spv);
    device->enabled_features = 0;
    cpci.stage.module = module;

    /* Capability bits that belong to another stage must not stop a compute
     * pipeline. The console platform declares multiview and the pinned CTS
     * enables the feature on every device it creates, so a compute-only case
     * carries a bit the compute adapter has no PSBC option for; refusing it
     * failed every compute pipeline on hardware as VK_ERROR_UNKNOWN from
     * vkCreateComputePipelines. */
    {
        VkPipeline with_multiview = VK_NULL_HANDLE;
        device->enabled_features = PS5VK_FEATURE_MULTIVIEW;
        assert(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                        &with_multiview) == VK_SUCCESS);
        assert(with_multiview != VK_NULL_HANDLE);
        assert(with_multiview->program.gfx == 1013);
        vkDestroyPipeline(device, with_multiview, NULL);

        /* The same pipeline with every currently declared capability bit set:
         * the two narrow-storage bits are mapped into compiler options and the
         * rest are irrelevant to this shader, so the combined mask must still
         * compile rather than trip the unknown-bit check. The optional-stage
         * bits are in the mask on purpose: the pinned CTS
         * enables every feature the device reports before it creates anything,
         * so as soon as one of them is advertised it arrives here on compute
         * pipelines too, and refusing it failed every compute lead on hardware
         * (VK_ERROR_UNKNOWN from vkCreateComputePipelines) until the compute
         * adapter learned to ignore bits that belong to another stage. This
         * test is the guard that failed first for the geometry bit, so it is
         * checked before the advertisement lands. The tessellation bit IS in
         * this mask now: its adapter path compiles the pair through the hull
         * and domain programs, and the compute whitelist learned the bit in
         * the same slice, so the guard checks the union rather than catching
         * a forgotten step. */
        const uint32_t all_declared = PS5VK_FEATURE_STORAGE_BUFFER_8BIT |
                                      PS5VK_FEATURE_STORAGE_BUFFER_16BIT |
                                      PS5VK_FEATURE_ROBUST_BUFFER_ACCESS |
                                      PS5VK_FEATURE_SHADER_DRAW_PARAMETERS |
                                      PS5VK_FEATURE_MULTIVIEW |
                                      PS5VK_FEATURE_SHADER_CLIP_DISTANCE |
                                      PS5VK_FEATURE_SHADER_CULL_DISTANCE |
                                      PS5VK_FEATURE_GEOMETRY_SHADER |
                                      PS5VK_FEATURE_TESSELLATION_SHADER;
        VkPipeline with_all = VK_NULL_HANDLE;
        device->enabled_features = all_declared;
        assert(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                        &with_all) == VK_SUCCESS);
        vkDestroyPipeline(device, with_all, NULL);

        /* Device scope without the base memory model is refused, and no
         * pipeline object is published for the invalid mask. */
        VkPipeline unknown_bit = (VkPipeline)(uintptr_t)1;
        device->enabled_features = all_declared |
                                   PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE;
        assert(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                        &unknown_bit) == VK_ERROR_UNKNOWN);
        assert(unknown_bit == VK_NULL_HANDLE);
        device->enabled_features = 0;
    }

    check_compute_execution_witness(device);

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
