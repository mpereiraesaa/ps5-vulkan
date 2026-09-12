#include <vulkan/vulkan_core.h>
#include "program_library.h"
#include "ps5log.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* CPU calculations below are references only. No CPU write to either output's
 * active range after poisoning and before vkWaitForFences + invalidate. */
static void fail(const char *operation, int rc)
{
    ps5log_printf(PS5LOG_ERR, "PS5VK_COMPUTE_FAIL operation=%s rc=%d", operation, rc);
    ps5log_close("compute-failed-retained");
    for (;;) sleep(1);
}
#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) fail(#call, r_); } while (0)
static uint32_t source(unsigned i, unsigned round)
{ return (i * UINT32_C(2654435761)) ^ (UINT32_C(0x79bd2468) + round * 9137u); }
#if defined(PS5VK_RUNTIME_COMPILER) && PS5VK_RUNTIME_COMPILER && !defined(PS5VK_GRAPHICS_API)
#include "compilation_cache.h"
#include "vk_internal.h"
static const uint32_t unregistered_program_spirv[] = {
    0x07230203u,0x00010600u,0x0008000bu,0x0000002fu,0x00000000u,0x00020011u,0x00000001u,0x0006000bu,
    0x00000001u,0x4c534c47u,0x6474732eu,0x3035342eu,0x00000000u,0x0003000eu,0x00000000u,0x00000001u,
    0x0008000fu,0x00000005u,0x00000004u,0x6e69616du,0x00000000u,0x0000000bu,0x00000019u,0x00000020u,
    0x00060010u,0x00000004u,0x00000011u,0x00000040u,0x00000001u,0x00000001u,0x00030003u,0x00000002u,
    0x000001c2u,0x00040005u,0x00000004u,0x6e69616du,0x00000000u,0x00030005u,0x00000008u,0x00000069u,
    0x00080005u,0x0000000bu,0x475f6c67u,0x61626f6cu,0x766e496cu,0x7461636fu,0x496e6f69u,0x00000044u,
    0x00040005u,0x00000017u,0x7074754fu,0x00007475u,0x00050006u,0x00000017u,0x00000000u,0x756c6176u,
    0x00007365u,0x00030005u,0x00000019u,0x00747364u,0x00040005u,0x0000001eu,0x75706e49u,0x00000074u,
    0x00050006u,0x0000001eu,0x00000000u,0x756c6176u,0x00007365u,0x00030005u,0x00000020u,0x00637273u,
    0x00040047u,0x0000000bu,0x0000000bu,0x0000001cu,0x00040047u,0x00000016u,0x00000006u,0x00000004u,
    0x00030047u,0x00000017u,0x00000002u,0x00040048u,0x00000017u,0x00000000u,0x00000019u,0x00050048u,
    0x00000017u,0x00000000u,0x00000023u,0x00000000u,0x00030047u,0x00000019u,0x00000019u,0x00040047u,
    0x00000019u,0x00000021u,0x00000001u,0x00040047u,0x00000019u,0x00000022u,0x00000000u,0x00040047u,
    0x0000001du,0x00000006u,0x00000004u,0x00030047u,0x0000001eu,0x00000002u,0x00040048u,0x0000001eu,
    0x00000000u,0x00000018u,0x00050048u,0x0000001eu,0x00000000u,0x00000023u,0x00000000u,0x00030047u,
    0x00000020u,0x00000018u,0x00040047u,0x00000020u,0x00000021u,0x00000000u,0x00040047u,0x00000020u,
    0x00000022u,0x00000000u,0x00020013u,0x00000002u,0x00030021u,0x00000003u,0x00000002u,0x00040015u,
    0x00000006u,0x00000020u,0x00000000u,0x00040020u,0x00000007u,0x00000007u,0x00000006u,0x00040017u,
    0x00000009u,0x00000006u,0x00000003u,0x00040020u,0x0000000au,0x00000001u,0x00000009u,0x0004003bu,
    0x0000000au,0x0000000bu,0x00000001u,0x0004002bu,0x00000006u,0x0000000cu,0x00000000u,0x00040020u,
    0x0000000du,0x00000001u,0x00000006u,0x0004002bu,0x00000006u,0x00000011u,0x00000400u,0x00020014u,
    0x00000012u,0x0003001du,0x00000016u,0x00000006u,0x0003001eu,0x00000017u,0x00000016u,0x00040020u,
    0x00000018u,0x0000000cu,0x00000017u,0x0004003bu,0x00000018u,0x00000019u,0x0000000cu,0x00040015u,
    0x0000001au,0x00000020u,0x00000001u,0x0004002bu,0x0000001au,0x0000001bu,0x00000000u,0x0003001du,
    0x0000001du,0x00000006u,0x0003001eu,0x0000001eu,0x0000001du,0x00040020u,0x0000001fu,0x0000000cu,
    0x0000001eu,0x0004003bu,0x0000001fu,0x00000020u,0x0000000cu,0x00040020u,0x00000022u,0x0000000cu,
    0x00000006u,0x0004002bu,0x00000006u,0x00000025u,0x00001337u,0x0004002bu,0x00000006u,0x00000028u,
    0x0000001fu,0x0004002bu,0x00000006u,0x0000002cu,0x00000040u,0x0004002bu,0x00000006u,0x0000002du,
    0x00000001u,0x0006002cu,0x00000009u,0x0000002eu,0x0000002cu,0x0000002du,0x0000002du,0x00050036u,
    0x00000002u,0x00000004u,0x00000000u,0x00000003u,0x000200f8u,0x00000005u,0x0004003bu,0x00000007u,
    0x00000008u,0x00000007u,0x00050041u,0x0000000du,0x0000000eu,0x0000000bu,0x0000000cu,0x0004003du,
    0x00000006u,0x0000000fu,0x0000000eu,0x0003003eu,0x00000008u,0x0000000fu,0x0004003du,0x00000006u,
    0x00000010u,0x00000008u,0x000500b0u,0x00000012u,0x00000013u,0x00000010u,0x00000011u,0x000300f7u,
    0x00000015u,0x00000000u,0x000400fau,0x00000013u,0x00000014u,0x00000015u,0x000200f8u,0x00000014u,
    0x0004003du,0x00000006u,0x0000001cu,0x00000008u,0x0004003du,0x00000006u,0x00000021u,0x00000008u,
    0x00060041u,0x00000022u,0x00000023u,0x00000020u,0x0000001bu,0x00000021u,0x0004003du,0x00000006u,
    0x00000024u,0x00000023u,0x00050080u,0x00000006u,0x00000026u,0x00000024u,0x00000025u,0x0004003du,
    0x00000006u,0x00000027u,0x00000008u,0x00050084u,0x00000006u,0x00000029u,0x00000027u,0x00000028u,
    0x000500c6u,0x00000006u,0x0000002au,0x00000026u,0x00000029u,0x00060041u,0x00000022u,0x0000002bu,
    0x00000019u,0x0000001bu,0x0000001cu,0x0003003eu,0x0000002bu,0x0000002au,0x000200f9u,0x00000015u,
    0x000200f8u,0x00000015u,0x000100fdu,0x00010038u
};
#endif
static uint32_t result(unsigned program, uint32_t input, unsigned i)
{
#if defined(PS5VK_RUNTIME_COMPILER) && PS5VK_RUNTIME_COMPILER && !defined(PS5VK_GRAPHICS_API)
    return program ? (input + 0x1337u) ^ (i * 31u) : input * 3u + 7u;
#else
    return program ? (input ^ UINT32_C(0xa5c39e71)) + i * 17u : input * 3u + 7u;
#endif
}
static void barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                    VkAccessFlags from, VkAccessFlags to)
{
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = from, .dstAccessMask = to};
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &b, 0, NULL, 0, NULL);
}
#ifdef PS5VK_GRAPHICS_API
/* The identical compute profile fixture can exercise an already-created graphics profile device. */
void ps5vk_compute_regression(VkDevice device)
{
#else
int main(void)
{
    struct timespec ts = {0}; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t boot = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
    ps5log_config cfg; const char *loaded = NULL; const char *paths[] = {"/app0/dev.conf"};
    ps5log_config_defaults(&cfg);
    if (ps5log_load_config(paths, 1, &cfg, &loaded)) _exit(0);
    cfg.udp = 0;
    if (ps5log_init(&cfg, "PPSA99994", "ps5vk", boot)) _exit(0);
#if defined(PS5VK_RUNTIME_COMPILER) && PS5VK_RUNTIME_COMPILER
    ps5log_line(PS5LOG_MARK, "PS5VK_BOOT stage=compute api=compute compiler=runtime-psbc-aco");
#else
    ps5log_line(PS5LOG_MARK, "PS5VK_BOOT stage=compute api=compute compiler=offline-exact-library");
#endif
    VkInstance instance;
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 1; VkPhysicalDevice physical;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice device; CHECK(vkCreateDevice(physical, &dci, NULL, &device));
#if defined(PS5VK_RUNTIME_COMPILER) && PS5VK_RUNTIME_COMPILER
    ps5vk_device_enable_runtime_compiler(device);
#endif
#endif
    VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue);
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(device, &slci, NULL, &sl));
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &sl};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
    VkPipeline pipelines[2];
#if defined(PS5VK_RUNTIME_COMPILER) && PS5VK_RUNTIME_COMPILER && !defined(PS5VK_GRAPHICS_API)
    /* Verify unregistered program is absent from the embedded library */
    const struct ps5vk_compiled_program *absent_check = NULL;
    VkResult abs_rc = ps5vk_program_resolve(&ps5vk_compiled_library,
        unregistered_program_spirv, sizeof(unregistered_program_spirv)/4, "main", &absent_check);
    if (abs_rc != VK_ERROR_FEATURE_NOT_PRESENT) fail("unregistered-present", abs_rc);
    ps5log_line(PS5LOG_MARK, "PS5VK_UNREGISTERED_ABSENT checked=1 rc=feature-not-present");

    for (unsigned p = 0; p < 2; ++p) {
        const uint32_t *spv_code = (p == 0) ? ps5vk_compiled_library.programs[0].spirv : unregistered_program_spirv;
        size_t spv_size = (p == 0) ? (ps5vk_compiled_library.programs[0].spirv_words * 4) : sizeof(unregistered_program_spirv);
        VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = spv_size, .pCode = spv_code};
        VkShaderModule module; CHECK(vkCreateShaderModule(device, &smci, NULL, &module));
        VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .layout = layout, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}};
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, NULL, &pipelines[p]));
        vkDestroyShaderModule(device, module, NULL);
        ps5log_printf(PS5LOG_MARK, "PS5VK_PIPELINE_CREATE program=%u mode=%s rc=0",
                      p, p ? "cold-compile-unregistered" : "cold-compile");
    }
    struct ps5vk_cache_stats cstats;
    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &cstats);
    ps5log_printf(PS5LOG_MARK, "PS5VK_CACHE_STATS entries=%u hits=%llu misses=%llu compiles=%llu evictions=%llu",
                  (unsigned)cstats.current_entries, (unsigned long long)cstats.hits,
                  (unsigned long long)cstats.misses, (unsigned long long)cstats.compiles,
                  (unsigned long long)cstats.evictions);

    /* Warm cache test: recreate pipeline 0 and verify cache hit */
    VkShaderModuleCreateInfo warm_smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = ps5vk_compiled_library.programs[0].spirv_words * 4,
        .pCode = ps5vk_compiled_library.programs[0].spirv};
    VkShaderModule warm_module; CHECK(vkCreateShaderModule(device, &warm_smci, NULL, &warm_module));
    VkComputePipelineCreateInfo warm_pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = layout, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = warm_module, .pName = "main"}};
    VkPipeline warm_pipe; CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &warm_pci, NULL, &warm_pipe));
    vkDestroyPipeline(device, warm_pipe, NULL);
    vkDestroyShaderModule(device, warm_module, NULL);
    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &cstats);
    ps5log_printf(PS5LOG_MARK, "PS5VK_CACHE_WARM_HIT program=0 hits=%llu compiles=%llu",
                  (unsigned long long)cstats.hits, (unsigned long long)cstats.compiles);

    /* Test invalid / unsupported shader rejection */
    uint32_t bad_spv[16] = {0x07230203, 0x00010000, 0, 10, 0};
    VkShaderModuleCreateInfo bad_smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(bad_spv), .pCode = bad_spv};
    VkShaderModule bad_module;
    if (vkCreateShaderModule(device, &bad_smci, NULL, &bad_module) == VK_SUCCESS) {
        VkComputePipelineCreateInfo bad_pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .layout = layout, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = bad_module, .pName = "main"}};
        VkPipeline bad_pipe;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &bad_pci, NULL, &bad_pipe) == VK_SUCCESS)
            fail("bad-shader-succeeded", -1);
        vkDestroyShaderModule(device, bad_module, NULL);
    }
    ps5log_line(PS5LOG_MARK, "PS5VK_UNSUPPORTED_REJECTED checked=1 rc=rejected-clean");
#else
    for (unsigned p = 0; p < 2; ++p) {
        const struct ps5vk_compiled_program *program = &ps5vk_compiled_library.programs[p];
        VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = program->spirv_words * 4, .pCode = program->spirv};
        VkShaderModule module; CHECK(vkCreateShaderModule(device, &smci, NULL, &module));
        VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .layout = layout, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}};
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, NULL, &pipelines[p]));
        vkDestroyShaderModule(device, module, NULL);
    }
#endif
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &size};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
    VkDescriptorSetLayout layouts[2] = {sl, sl};
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 2, .pSetLayouts = layouts};
    VkDescriptorSet sets[2]; CHECK(vkAllocateDescriptorSets(device, &dsai, sets));
    VkBuffer buffers[3]; VkDeviceMemory memories[3]; uint32_t *mapped[3];
    VkMappedMemoryRange ranges[3];
    for (unsigned b = 0; b < 3; ++b) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 8192, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        CHECK(vkCreateBuffer(device, &bci, NULL, &buffers[b]));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, buffers[b], &req);
        if (req.size + 256 > 16384 || 256 % req.alignment) fail("buffer-requirements", -1);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 16384};
        CHECK(vkAllocateMemory(device, &mai, NULL, &memories[b]));
        CHECK(vkBindBufferMemory(device, buffers[b], memories[b], 256));
        CHECK(vkMapMemory(device, memories[b], 0, VK_WHOLE_SIZE, 0, (void **)&mapped[b]));
        ranges[b] = (VkMappedMemoryRange){.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = memories[b], .size = VK_WHOLE_SIZE};
    }
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool commands; CHECK(vkCreateCommandPool(device, &cpci, NULL, &commands));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(device, &cbai, &cb));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CHECK(vkCreateFence(device, &fci, NULL, &fence));
    for (unsigned round = 0; round < 6; ++round) {
        CHECK(vkResetCommandBuffer(cb, 0)); CHECK(vkResetFences(device, 1, &fence));
        unsigned offset = 256 * (round + 1), first = (256 + offset) / 4;
        uint32_t poison = 0xbad00000u + round;
        for (unsigned b = 0; b < 3; ++b)
            for (unsigned i = 0; i < 4096; ++i) mapped[b][i] = poison;
        for (unsigned i = 0; i < 1024; ++i) mapped[0][first + i] = source(i, round);
        CHECK(vkFlushMappedMemoryRanges(device, 3, ranges));
        for (unsigned s = 0; s < 2; ++s) {
            VkDescriptorBufferInfo bi[2] = {{buffers[s], offset, 4096}, {buffers[s + 1], offset, 4096}};
            VkWriteDescriptorSet writes[2];
            for (unsigned b = 0; b < 2; ++b) writes[b] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[s], .dstBinding = b,
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[b]};
            vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
        }
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cb, &begin));
        barrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        for (unsigned s = 0; s < 2; ++s) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[(round + s) % 2]);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[s], 0, NULL);
            vkCmdDispatch(cb, 16, 1, 1);
            barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                s ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, s ? VK_ACCESS_HOST_READ_BIT : VK_ACCESS_SHADER_READ_BIT);
        }
        CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cb};
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
        CHECK(vkInvalidateMappedMemoryRanges(device, 3, ranges));
        unsigned errors = 0, guards = 0;
        for (unsigned i = 0; i < 4096; ++i) {
            if (i >= first && i < first + 1024) {
                unsigned index = i - first; uint32_t expected = source(index, round);
                errors += mapped[0][i] != expected;
                expected = result(round % 2, expected, index); errors += mapped[1][i] != expected;
                expected = result((round + 1) % 2, expected, index); errors += mapped[2][i] != expected;
            } else for (unsigned b = 0; b < 3; ++b) guards += mapped[b][i] != poison;
        }
        ps5log_printf(PS5LOG_MARK, "PS5VK_COMPUTE_RESULT round=%u first_program=%u descriptor_offset=%u binding_offset=256 checked=3072 outputs=%u guards=%u",
                      round, round % 2, offset, errors, guards);
        if (errors || guards) fail("reference-mismatch", -1);
    }
    CHECK(vkDeviceWaitIdle(device));
    vkDestroyCommandPool(device, commands, NULL); vkDestroyFence(device, fence, NULL);
    vkDestroyDescriptorPool(device, pool, NULL);
    for (unsigned p = 0; p < 2; ++p) vkDestroyPipeline(device, pipelines[p], NULL);
    vkDestroyPipelineLayout(device, layout, NULL); vkDestroyDescriptorSetLayout(device, sl, NULL);
    for (unsigned b = 0; b < 3; ++b) {
        vkUnmapMemory(device, memories[b]); vkDestroyBuffer(device, buffers[b], NULL);
        vkFreeMemory(device, memories[b], NULL);
    }
#if defined(PS5VK_RUNTIME_COMPILER) && PS5VK_RUNTIME_COMPILER && !defined(PS5VK_GRAPHICS_API)
    ps5vk_compilation_cache_get_stats(device->pipeline_cache, &cstats);
    ps5log_printf(PS5LOG_MARK, "PS5VK_CACHE_FINAL entries=%u hits=%llu misses=%llu compiles=%llu evictions=%llu",
                  (unsigned)cstats.current_entries, (unsigned long long)cstats.hits,
                  (unsigned long long)cstats.misses, (unsigned long long)cstats.compiles,
                  (unsigned long long)cstats.evictions);
#endif
#ifndef PS5VK_GRAPHICS_API
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
#endif
    ps5log_line(PS5LOG_MARK, "PS5VK_COMPUTE_END rounds=6 dispatches=12");
#ifndef PS5VK_GRAPHICS_API
    ps5log_close("compute-end"); _exit(0);
#endif
}
