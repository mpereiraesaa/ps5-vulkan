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
static uint32_t result(unsigned program, uint32_t input, unsigned i)
{ return program ? (input ^ UINT32_C(0xa5c39e71)) + i * 17u : input * 3u + 7u; }
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
    ps5log_line(PS5LOG_MARK, "PS5VK_BOOT stage=compute api=compute compiler=offline-exact-library");
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
#ifndef PS5VK_GRAPHICS_API
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
#endif
    ps5log_line(PS5LOG_MARK, "PS5VK_COMPUTE_END rounds=6 dispatches=12");
#ifndef PS5VK_GRAPHICS_API
    ps5log_close("compute-end"); _exit(0);
#endif
}
