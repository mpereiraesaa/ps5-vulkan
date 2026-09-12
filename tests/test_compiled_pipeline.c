#include "program_library.h"
#include "dispatch_encode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct VkDevice_T d = {.compiler = {&ps5vk_compiled_library, ps5vk_program_resolve}};
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings}; VkDescriptorSetLayout set;
    assert(vkCreateDescriptorSetLayout(&d, &si, NULL, &set) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set}; VkPipelineLayout layout;
    assert(vkCreatePipelineLayout(&d, &li, NULL, &layout) == VK_SUCCESS);
    vkDestroyDescriptorSetLayout(&d, set, NULL);
    assert(ps5vk_compiled_library.count == 2);
    VkPipeline pipelines[2];
    for (size_t j = 0; j < 2; ++j) {
        const struct ps5vk_compiled_program *program = &compiled_programs[j];
        VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = program->spirv_words * 4, .pCode = program->spirv}; VkShaderModule module;
        assert(vkCreateShaderModule(&d, &mi, NULL, &module) == VK_SUCCESS);
        VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .layout = layout, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}};
        assert(vkCreateComputePipelines(&d, VK_NULL_HANDLE, 1, &pi, NULL, &pipelines[j]) == VK_SUCCESS);
        assert(pipelines[j]->program.code_words == program->code_words);
        assert(!memcmp(pipelines[j]->code, program->code, program->code_words * 4));
        struct ps5vk_dispatch_encoding encoding = {.program = &pipelines[j]->program,
            .addresses = {0x100004000 + j * 0x1000, 0x100008000, 0x200000000, 0x200000040},
            .groups = {16, 1, 1}, .completion_value = j + 1};
        uint32_t packet[PS5VK_COMPUTE_COMMAND_CAPACITY];
        assert(ps5vk_dispatch_encode(packet, PS5VK_COMPUTE_COMMAND_CAPACITY, &encoding) == 82);
        assert(packet[12] == (uint32_t)(encoding.addresses.code >> 8));
        assert(packet[79] == j + 1);
        module->words[2] ^= 0x80000000u; /* Different module identity must not hit another program. */
        VkPipeline rejected;
        assert(vkCreateComputePipelines(&d, VK_NULL_HANDLE, 1, &pi, NULL, &rejected) == VK_ERROR_UNKNOWN);
        assert(!rejected); vkDestroyShaderModule(&d, module, NULL);
    }
    assert(pipelines[0]->program.code_words != pipelines[1]->program.code_words ||
           memcmp(pipelines[0]->code, pipelines[1]->code, pipelines[0]->program.code_words * 4));
    vkDestroyPipelineLayout(&d, layout, NULL);
    vkDestroyPipeline(&d, pipelines[0], NULL); vkDestroyPipeline(&d, pipelines[1], NULL);
    assert(!d.pipeline_objects && !d.descriptor_objects);
    puts("Two real LLPC modules -> distinct pipelines -> dispatch packets: pass (host only; not GPU execution)");
}
