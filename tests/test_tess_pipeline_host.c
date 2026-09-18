/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The tessellation pipeline's full create path on the host: the real
 * vkCreateGraphicsPipelines against the runtime graphics adapter, the same
 * acquire the payload uses, and the native create's own validation up to the
 * AGC calls (which are console-only and stubbed here). The witness run showed
 * the create failing with VK_ERROR_FEATURE_NOT_PRESENT before any adapter
 * diagnostic fired; this test reproduces the create with the same key shape
 * so the failing stage is local and debuggable.
 */
#include "vk_internal.h"
#include "physical_device_profile.h"
#include "graphics_pipeline_ps5.h"
#include "vk_internal.h"
#include "physical_device_profile.h"
#include "graphics_pipeline_ps5.h"
#include "runtime_graphics_compiler.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkResult alloc_memory(void *context, VkDeviceSize size,
                             void **address, void **backing)
{
    (void)context;
    void *block = malloc((size_t)size);
    if (!block) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(block, 0, (size_t)size);
    *address = block;
    *backing = block;
    return VK_SUCCESS;
}
static void free_memory(void *context, void *backing)
{
    (void)context;
    free(backing);
}
static VkResult cache_flush(void *context, void *backing, uint64_t offset,
                            uint64_t size)
{
    (void)context; (void)backing; (void)offset; (void)size;
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }

VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = NULL, .close = close_backend,
                                 .max_allocation = 1u << 24,
                                 .queue_flags = VK_QUEUE_GRAPHICS_BIT |
                                                VK_QUEUE_COMPUTE_BIT};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU",
        .vendor_id = 0x1002,
        .device_id = 0x73a0,
        .heap_size = 1u << 26,
        .allocation_granularity = 1,
        .buffer_image_granularity = 1,
        .host_coherent = VK_TRUE,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

static uint32_t *read_spv(const char *path, size_t *words)
{
    FILE *f=fopen(path,"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long b=ftell(f);assert(b>0);
    rewind(f);uint32_t *w=malloc((size_t)b);assert(w);
    assert(fread(w,1,(size_t)b,f)==(size_t)b);fclose(f);
    *words=(size_t)b/4;return w;
}

int main(void)
{
    struct { const char *path; VkShaderModule module; size_t words; uint32_t *code; }
        modules[4]={
            {"build/runtime-graphics/tess_witness.vert.spv"},
            {"build/runtime-graphics/tess_witness_control.spv"},
            {"build/runtime-graphics/tess_witness_evaluation.spv"},
            {"build/runtime-graphics/tess_witness_fragment.spv"}};
    VkInstance instance;
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&ici,NULL,&instance)==VK_SUCCESS);
    uint32_t count=1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance,&count,&physical)==VK_SUCCESS);
    float priority=1.0f;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice device;
    assert(vkCreateDevice(physical,&dci,NULL,&device)==VK_SUCCESS);

    /* The runtime graphics adapter, exactly as the payload configures it. */
    device->graphics_enabled=VK_TRUE;
    device->memory.allocate=alloc_memory;
    device->memory.release=free_memory;
    device->memory.flush=cache_flush;
    device->graphics_compiler_context=NULL;
    device->graphics_acquire=ps5vk_runtime_graphics_cached_acquire;
    device->graphics_compiled_release=ps5vk_runtime_graphics_cached_release;
    device->graphics_create=ps5vk_native_runtime_graphics_create;
    device->graphics_release=ps5vk_native_graphics_release;

    VkShaderModule shader_modules[4];
    const VkShaderStageFlagBits stages_of[4]={
        VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,VK_SHADER_STAGE_FRAGMENT_BIT};
    for(unsigned i=0;i<4;++i) {
        modules[i].code=read_spv(modules[i].path,&modules[i].words);
        VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=modules[i].words*4,.pCode=modules[i].code};
        assert(vkCreateShaderModule(device,&mi,NULL,&shader_modules[i])==VK_SUCCESS);
    }
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    assert(vkCreatePipelineLayout(device,&li,NULL,&layout)==VK_SUCCESS);

    VkPipelineShaderStageCreateInfo stage_infos[4];
    for(unsigned i=0;i<4;++i)
        stage_infos[i]=(VkPipelineShaderStageCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage=stages_of[i],.module=shader_modules[i],.pName="main",
            .flags=0,.pNext=NULL,.pSpecializationInfo=NULL};
    VkPipelineInputAssemblyStateCreateInfo ia={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineTessellationStateCreateInfo ts={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .patchControlPoints=3};
    VkPipelineVertexInputStateCreateInfo vi={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo raster={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkViewport viewport={0,0,64,64,0,1};
    VkRect2D scissor={{0,0},{64,64}};
    VkPipelineViewportStateCreateInfo vp={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineColorBlendAttachmentState blend_a={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend={
        .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend_a};
    VkGraphicsPipelineCreateInfo pi={
        .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .layout=layout,.stageCount=4,.pStages=stage_infos,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,
        .pTessellationState=&ts,.pRasterizationState=&raster,
        .pMultisampleState=&ms,.pViewportState=&vp,.pColorBlendState=&blend};

    /* A render pass is required by the create's format identity. */
    VkAttachmentDescription attachment={
        .format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference reference={.attachment=0,
        .layout=VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&reference};
    VkRenderPassCreateInfo rpi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,
        .pSubpasses=&subpass};
    VkRenderPass pass;
    assert(vkCreateRenderPass(device,&rpi,NULL,&pass)==VK_SUCCESS);
    pi.renderPass=pass;

    extern unsigned ps5vk_pipeline_refusal_site_value_state(void);
    const unsigned refusal_before=ps5vk_pipeline_refusal_site_value_state();
    VkPipeline pipeline=VK_NULL_HANDLE;
    const VkResult rc=vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&pipeline);
    printf("tess pipeline create: rc=%d site=%u (was %u) pipeline=%p\n",
        (int)rc,ps5vk_pipeline_refusal_site_value_state(),refusal_before,
        (void*)pipeline);
    if(pipeline)vkDestroyPipeline(device,pipeline,NULL);

    for(unsigned i=0;i<4;++i)vkDestroyShaderModule(device,shader_modules[i],NULL);
    vkDestroyRenderPass(device,pass,NULL);
    vkDestroyPipelineLayout(device,layout,NULL);
    vkDestroyDevice(device,NULL);
    vkDestroyInstance(instance,NULL);
    for(unsigned i=0;i<4;++i)free(modules[i].code);
    puts("Tessellation host create: full path exercised");
    return 0;
}
