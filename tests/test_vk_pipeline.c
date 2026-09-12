#include "vk_pipeline.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Structural state-machine fixtures, not valid executable GPU programs. Real
 * LLPC artifact integration is a separate compiler-dependent test. */
static uint32_t module_a[] = {0x07230203, 0x10000, 0, 2, 0,
    (5u << 16) | 15, 5, 1, 0x6e69616d, 0, (6u << 16) | 16, 1, 17, 64, 1, 1};
static uint32_t module_b[] = {0x07230203, 0x10000, 1, 2, 0,
    (5u << 16) | 15, 5, 1, 0x6e69616d, 0, (6u << 16) | 16, 1, 17, 64, 1, 1};
static uint32_t code_a[] = {0x11111111}, code_b[] = {0x22222222};
static struct ps5vk_compiled_program fixture(const uint32_t *module, const uint32_t *code)
{
    return (struct ps5vk_compiled_program){.spirv = module, .spirv_words = 16,
        .code = code, .code_words = 1, .entry = "main", .gfx = 1013,
        .local_size = {64, 1, 1}, .wave_size = 32, .vgprs = 3, .sgprs = 10,
        .float_mode = 192, .mem_ordered = 1, .user_sgprs = 2, .tg_size = 1,
        .tgid = {1, 1, 1}, .descriptor_count = 1, .descriptors = {{0, 0, 0, 0}}};
}
static VkShaderModule shader(VkDevice d, const uint32_t *words)
{
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(module_a), .pCode = words}; VkShaderModule m;
    assert(vkCreateShaderModule(d, &info, NULL, &m) == VK_SUCCESS);
    return m;
}
static VkPipelineLayout layout(VkDevice d)
{
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding}; VkDescriptorSetLayout set;
    assert(vkCreateDescriptorSetLayout(d, &info, NULL, &set) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set}; VkPipelineLayout result;
    assert(vkCreatePipelineLayout(d, &pi, NULL, &result) == VK_SUCCESS);
    vkDestroyDescriptorSetLayout(d, set, NULL); return result;
}
static VkComputePipelineCreateInfo info(VkShaderModule m, VkPipelineLayout l)
{
    return (VkComputePipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = l, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
}
static void lifecycle(void)
{
    struct ps5vk_compiled_program programs[] = {fixture(module_a, code_a), fixture(module_b, code_b)};
    struct ps5vk_program_library lib = {programs, 2};
    struct VkDevice_T d = {.compiler = {&lib, ps5vk_program_resolve}};
    VkShaderModule a = shader(&d, module_a), b = shader(&d, module_b);
    module_a[2] = 99; assert(a->words[2] == 0); module_a[2] = 0; /* Owned module bytes. */
    VkPipelineLayout l = layout(&d);
    VkComputePipelineCreateInfo infos[] = {info(a, l), info(b, l)}; VkPipeline pipelines[2];
    assert(vkCreateComputePipelines(&d, VK_NULL_HANDLE, 2, infos, NULL, pipelines) == VK_SUCCESS);
    assert(pipelines[0]->code[0] != pipelines[1]->code[0]);
    assert(pipelines[0]->program.code == pipelines[0]->code && !pipelines[0]->program.spirv);
    code_a[0] = 0; assert(pipelines[0]->code[0] == 0x11111111); code_a[0] = 0x11111111;
    vkDestroyShaderModule(&d, a, NULL); vkDestroyShaderModule(&d, b, NULL);
    vkDestroyPipelineLayout(&d, l, NULL);
    assert(pipelines[0]->sets[0].binding[0].count == 1 && d.pipeline_objects == 2);
    pipelines[0]->pending = 1; vkDestroyPipeline(&d, pipelines[0], NULL);
    assert(d.pipeline_objects == 2 && d.lifetime_errors == 1);
    pipelines[0]->pending = 0;
    vkDestroyPipeline(&d, pipelines[0], NULL); vkDestroyPipeline(&d, pipelines[1], NULL);
    assert(!d.pipeline_objects && !d.descriptor_objects);
}
static void negative(void)
{
    struct ps5vk_compiled_program p = fixture(module_a, code_a);
    struct ps5vk_program_library lib = {&p, 1}; struct VkDevice_T d = {.compiler = {&lib, ps5vk_program_resolve}};
    VkShaderModule a = shader(&d, module_a), b = shader(&d, module_b); VkPipelineLayout l = layout(&d);
    VkComputePipelineCreateInfo infos[] = {info(a, l), info(b, l)}; VkPipeline out[2];
    assert(vkCreateComputePipelines(&d, VK_NULL_HANDLE, 2, infos, NULL, out) == VK_ERROR_UNKNOWN);
    assert(out[0] && !out[1]); vkDestroyPipeline(&d, out[0], NULL);
    p.descriptors[0].binding = 1;
    assert(vkCreateComputePipelines(&d, VK_NULL_HANDLE, 1, infos, NULL, out) != VK_SUCCESS && !out[0]);
    p.descriptors[0].binding = 0; p.local_size[0] = 32;
    assert(vkCreateComputePipelines(&d, VK_NULL_HANDLE, 1, infos, NULL, out) != VK_SUCCESS);
    p.local_size[0] = 64; p.gfx = 1030;
    assert(vkCreateComputePipelines(&d, VK_NULL_HANDLE, 1, infos, NULL, out) != VK_SUCCESS);
    p.gfx = 1013; infos[0].stage.pName = "other";
    assert(vkCreateComputePipelines(&d, VK_NULL_HANDLE, 1, infos, NULL, out) != VK_SUCCESS);
    infos[0].stage.pName = "main"; d.compiler.resolve = NULL;
    assert(vkCreateComputePipelines(&d, VK_NULL_HANDLE, 1, infos, NULL, out) == VK_ERROR_UNKNOWN);
    VkShaderModuleCreateInfo bad = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(module_a), .pCode = module_a}; VkShaderModule rejected;
    module_a[5] = 15;
    assert(vkCreateShaderModule(&d, &bad, NULL, &rejected) != VK_SUCCESS && !rejected);
    module_a[5] = (5u << 16) | 15;
    vkDestroyShaderModule(&d, a, NULL); vkDestroyShaderModule(&d, b, NULL); vkDestroyPipelineLayout(&d, l, NULL);
    assert(!d.pipeline_objects && !d.descriptor_objects);
}
static void graphics_entries(void)
{
    /* Structural fixture: two different execution models may share a name. */
    uint32_t words[] = {0x07230203, 0x10000, 0, 3, 0,
        (5u << 16) | 15, 0, 1, 0x6e69616d, 0,
        (5u << 16) | 15, 4, 2, 0x6e69616d, 0};
    struct VkDevice_T d = {0};
    VkShaderModuleCreateInfo info = {.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(words), .pCode=words};
    VkShaderModule module; uint32_t id;
    assert(vkCreateShaderModule(&d, &info, NULL, &module) == VK_SUCCESS);
    assert(ps5vk_shader_entry(module, VK_SHADER_STAGE_VERTEX_BIT, "main", &id) && id == 1);
    assert(ps5vk_shader_entry(module, VK_SHADER_STAGE_FRAGMENT_BIT, "main", &id) && id == 2);
    assert(!ps5vk_shader_entry(module, VK_SHADER_STAGE_COMPUTE_BIT, "main", &id) && !id);
    assert(!ps5vk_shader_entry(module, VK_SHADER_STAGE_FRAGMENT_BIT, "other", &id));
    module->words[11] = 0; /* Duplicate vertex/main is ambiguous. */
    assert(!ps5vk_shader_entry(module, VK_SHADER_STAGE_VERTEX_BIT, "main", &id));
    module->words[11] = 4; module->words[12] = 3;
    assert(!ps5vk_shader_entry(module, VK_SHADER_STAGE_FRAGMENT_BIT, "main", &id));
    module->words[12] = 2; module->words[14] = 0xffffffff;
    assert(!ps5vk_shader_entry(module, VK_SHADER_STAGE_FRAGMENT_BIT, "main", &id));
    vkDestroyShaderModule(&d, module, NULL); assert(!d.pipeline_objects);
}
int main(void)
{
    lifecycle(); negative(); graphics_entries(); puts("Shader/pipeline contracts: pass (synthetic, no GPU execution)");
}
