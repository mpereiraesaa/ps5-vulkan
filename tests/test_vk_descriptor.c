#include "descriptor_table_layout.h"
#include "vk_descriptor.h"
#include "vk_image.h"
#include "vk_sampler.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct counts { unsigned live; int remaining; };
static void *VKAPI_CALL allocate(void *ctx, size_t size, size_t alignment, VkSystemAllocationScope scope)
{
    struct counts *c = ctx;
    assert(alignment <= _Alignof(max_align_t) && scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!c->remaining) return NULL;
    if (c->remaining > 0) --c->remaining;
    void *p = malloc(size); assert(p); ++c->live; return p;
}
static void *VKAPI_CALL reallocate(void *ctx, void *p, size_t n, size_t a, VkSystemAllocationScope s)
{ (void)ctx; (void)a; (void)s; return realloc(p, n); }
static void VKAPI_CALL release(void *ctx, void *p)
{ struct counts *c = ctx; assert(c->live); --c->live; free(p); }
static void *VKAPI_CALL poison_allocate(void *ctx, size_t size, size_t alignment,
                                        VkSystemAllocationScope scope)
{
    void *p=allocate(ctx,size,alignment,scope);
    if(p)memset(p,0xa5,size);
    return p;
}
static VkDescriptorSetLayout layout(VkDevice d)
{
    VkDescriptorSetLayoutBinding bindings[] = {
        {.binding = 7, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 2, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout result;
    assert(vkCreateDescriptorSetLayout(d, &ci, NULL, &result) == VK_SUCCESS);
    assert(result->signature.binding[1].first == 0 && result->signature.binding[7].first == 1);
    return result;
}
static VkDescriptorPool pool(VkDevice d, uint32_t sets, uint32_t descriptors,
                              VkDescriptorPoolCreateFlags flags, const VkAllocationCallbacks *a)
{
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptors}};
    VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = flags, .maxSets = sets, .poolSizeCount = 1, .pPoolSizes = sizes};
    VkDescriptorPool result;
    assert(vkCreateDescriptorPool(d, &ci, a, &result) == VK_SUCCESS);
    return result;
}
static void lifecycle(void)
{
    struct VkDevice_T d = {0}, other = {0};
    VkDescriptorSetLayout l = layout(&d), layouts[] = {l, l};
    VkDescriptorPool p = pool(&d, 2, 6, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, NULL);
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p, .descriptorSetCount = 2, .pSetLayouts = layouts};
    VkDescriptorSet sets[2];
    assert(vkAllocateDescriptorSets(&other, &ai, sets) != VK_SUCCESS && !sets[0] && !sets[1]);
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_SUCCESS);
    assert(p->used_sets == 2 && p->storage_used == 6);
    for (unsigned j = 0; j < PS5VK_MAX_DESCRIPTORS; ++j) assert(!sets[0]->defined[j]);
    VkDescriptorSet extra[2];
    assert(vkAllocateDescriptorSets(&d, &ai, extra) == VK_ERROR_OUT_OF_POOL_MEMORY);
    assert(!extra[0] && !extra[1] && p->used_sets == 2);
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2, .pSetLayouts = layouts};
    VkPipelineLayout pipeline;
    assert(vkCreatePipelineLayout(&d, &pi, NULL, &pipeline) == VK_SUCCESS);
    vkDestroyDescriptorSetLayout(&d, l, NULL);
    assert(pipeline->sets[1].count == 3 && sets[1]->signature.binding[7].count == 2);
    VkDescriptorSet duplicate[] = {sets[0], sets[0]};
    assert(vkFreeDescriptorSets(&d, p, 2, duplicate) != VK_SUCCESS && p->used_sets == 2);
    sets[1]->pending = 1;
    assert(vkFreeDescriptorSets(&d, p, 2, sets) != VK_SUCCESS && p->used_sets == 2);
    assert(vkResetDescriptorPool(&d, p, 0) != VK_SUCCESS);
    vkDestroyDescriptorPool(&d, p, NULL); assert(d.lifetime_errors == 1 && p->used_sets == 2);
    sets[1]->pending = 0;
    assert(vkFreeDescriptorSets(&d, p, 1, sets) == VK_SUCCESS && p->storage_used == 3);
    assert(vkResetDescriptorPool(&d, p, 0) == VK_SUCCESS && !p->used_sets && !p->storage_used);
    vkDestroyDescriptorPool(&d, p, NULL); vkDestroyPipelineLayout(&d, pipeline, NULL);
    assert(!d.descriptor_objects);
}
static void rollback(void)
{
    struct VkDevice_T d = {0}; struct counts counts = {.remaining = -1};
    VkAllocationCallbacks a = {.pUserData = &counts, .pfnAllocation = allocate,
        .pfnReallocation = reallocate, .pfnFree = release};
    VkDescriptorSetLayout l = layout(&d), layouts[] = {l, l};
    VkDescriptorPool p = pool(&d, 4, 6, 0, &a);
    assert(counts.live == 1);
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p, .descriptorSetCount = 2, .pSetLayouts = layouts};
    VkDescriptorSet sets[2]; counts.remaining = 1;
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(!sets[0] && !sets[1] && !p->used_sets && !p->storage_used && counts.live == 1);
    counts.remaining = -1;
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_SUCCESS && counts.live == 3);
    assert(vkFreeDescriptorSets(&d, p, 2, sets) != VK_SUCCESS); /* Pool lacks FREE flag. */
    VkDescriptorSet extra[2];
    assert(vkAllocateDescriptorSets(&d, &ai, extra) == VK_ERROR_OUT_OF_POOL_MEMORY); /* Descriptor budget. */
    assert(vkResetDescriptorPool(&d, p, 0) == VK_SUCCESS && counts.live == 1);
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_SUCCESS);
    vkDestroyDescriptorPool(&d, p, &a); vkDestroyDescriptorSetLayout(&d, l, NULL);
    assert(!counts.live && !d.descriptor_objects);
}
static void negative(void)
{
    struct VkDevice_T d = {0};
    VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}
    };
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings}; VkDescriptorSetLayout l;
    assert(vkCreateDescriptorSetLayout(&d, &ci, NULL, &l) != VK_SUCCESS && !l);
    ci.bindingCount = 1; bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    assert(vkCreateDescriptorSetLayout(&d, &ci, NULL, &l) == VK_ERROR_FEATURE_NOT_PRESENT);
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = PS5VK_MAX_DESCRIPTORS + 1;
    assert(vkCreateDescriptorSetLayout(&d, &ci, NULL, &l) == VK_ERROR_FEATURE_NOT_PRESENT);
    bindings[0].descriptorCount = 0;
    assert(vkCreateDescriptorSetLayout(&d, &ci, NULL, &l) == VK_SUCCESS);
    vkDestroyDescriptorSetLayout(&d, l, NULL);
    bindings[0].descriptorCount = 1;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
    assert(vkCreateDescriptorSetLayout(&d, &ci, NULL, &l) == VK_SUCCESS);
    vkDestroyDescriptorSetLayout(&d, l, NULL);
    bindings[0].stageFlags = 0;
    assert(vkCreateDescriptorSetLayout(&d, &ci, NULL, &l) != VK_SUCCESS && !l);
    bindings[0].stageFlags = UINT32_C(0x40000000);
    assert(vkCreateDescriptorSetLayout(&d, &ci, NULL, &l) != VK_SUCCESS && !l);
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0]=(VkDescriptorSetLayoutBinding){0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,
        VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    assert(vkCreateDescriptorSetLayout(&d,&ci,NULL,&l)==VK_ERROR_FEATURE_NOT_PRESENT && !l);
    d.uniform_buffer_alignment=256;
    assert(vkCreateDescriptorSetLayout(&d,&ci,NULL,&l)==VK_SUCCESS);
    vkDestroyDescriptorSetLayout(&d,l,NULL);
    assert(!d.descriptor_objects);
}
static void bda_cts_output_layout(void)
{
    /* The original BDA compute cases declare a storage-image result at
     * binding 0 and an SSBO at binding 1, both visible to all three stages.
     * The image uses its own pool budget and a resource-only table record. */
    struct VkDevice_T d = {.graphics_enabled = VK_TRUE};
    VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT |
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding bindings[] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1, .stageFlags = stages},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = stages},
    };
    VkDescriptorSetLayoutCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    assert(vkCreateDescriptorSetLayout(&d, &ci, NULL, &layout) == VK_SUCCESS);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    VkDescriptorPool p=VK_NULL_HANDLE;
    assert(vkCreateDescriptorPool(&d,&pi,NULL,&p)==VK_SUCCESS);
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=p,.descriptorSetCount=1,.pSetLayouts=&layout};
    VkDescriptorSet set=VK_NULL_HANDLE;
    assert(vkAllocateDescriptorSets(&d,&ai,&set)==VK_SUCCESS);
    assert(p->storage_image_used==1 && p->storage_used==1);
    assert(set->signature.binding[0].first==0 && set->signature.binding[1].first==1);
    struct VkImage_T image={.device=&d,.info={.usage=VK_IMAGE_USAGE_STORAGE_BIT}};
    struct VkImageView_T view={.device=&d,.image=&image,
        .view_type=VK_IMAGE_VIEW_TYPE_2D};
    VkDescriptorImageInfo descriptor={.sampler=(VkSampler)(uintptr_t)1,
        .imageView=&view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=set,.dstBinding=0,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&descriptor};
    vkUpdateDescriptorSets(&d,1,&write,0,NULL);
    assert(set->defined[0] && set->images[0].sampler==VK_NULL_HANDLE &&
        set->image_resources[0]==&image && !d.lifetime_errors);
    descriptor.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkUpdateDescriptorSets(&d,1,&write,0,NULL);
    assert(d.lifetime_errors==1 && set->images[0].imageLayout==VK_IMAGE_LAYOUT_GENERAL);
    d.lifetime_errors=0;
    vkDestroyDescriptorPool(&d,p,NULL);
    vkDestroyDescriptorSetLayout(&d,layout,NULL);
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    assert(vkCreateDescriptorSetLayout(&d, &ci, NULL, &layout) == VK_SUCCESS);
    vkDestroyDescriptorSetLayout(&d, layout, NULL);
    assert(!d.descriptor_objects);
}
static void push_constant_layouts(void)
{
    struct VkDevice_T d={0};
    struct counts counts={.remaining=-1};
    VkAllocationCallbacks poisoned={.pUserData=&counts,.pfnAllocation=poison_allocate,
        .pfnReallocation=reallocate,.pfnFree=release};
    VkPipelineLayoutCreateInfo empty={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout clean;
    assert(vkCreatePipelineLayout(&d,&empty,&poisoned,&clean)==VK_SUCCESS);
    assert(clean->set_count==0 && clean->push_constant_size==0);
    for(unsigned i=0;i<PS5VK_MAX_PUSH_CONSTANT_DWORDS;++i)
        assert(clean->push_constant_stages[i]==0);
    vkDestroyPipelineLayout(&d,clean,&poisoned);assert(!counts.live);
    VkPushConstantRange ranges[]={
        {VK_SHADER_STAGE_COMPUTE_BIT,0,16},
        {VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,16,16}};
    VkPipelineLayoutCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount=2,.pPushConstantRanges=ranges};
    VkPipelineLayout layout;
    assert(vkCreatePipelineLayout(&d,&info,NULL,&layout)==VK_SUCCESS);
    assert(layout->push_constant_size==32);
    for(unsigned i=0;i<4;++i)assert(layout->push_constant_stages[i]==VK_SHADER_STAGE_COMPUTE_BIT);
    for(unsigned i=4;i<8;++i)assert(layout->push_constant_stages[i]==
        (VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT));
    vkDestroyPipelineLayout(&d,layout,NULL);
    /* VUID00292 forbids a stage appearing in two ranges even when their
     * byte intervals are disjoint. Distinct stages may overlap instead. */
    const VkShaderStageFlags stages[]={VK_SHADER_STAGE_VERTEX_BIT,
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,VK_SHADER_STAGE_GEOMETRY_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT,VK_SHADER_STAGE_COMPUTE_BIT};
    for(unsigned s=0;s<sizeof(stages)/sizeof(stages[0]);++s) {
        ranges[0]=(VkPushConstantRange){stages[s],0,16};
        ranges[1]=(VkPushConstantRange){stages[s],32,16};
        layout=(VkPipelineLayout)(uintptr_t)1;
        assert(vkCreatePipelineLayout(&d,&info,&poisoned,&layout)!=VK_SUCCESS && !layout);
        assert(!d.descriptor_objects && !counts.live);
        ranges[1].stageFlags=stages[(s+1)%6];
        ranges[1].offset=0;
        assert(vkCreatePipelineLayout(&d,&info,&poisoned,&layout)==VK_SUCCESS);
        assert(layout->push_constant_stages[0]==(stages[s]|stages[(s+1)%6]));
        vkDestroyPipelineLayout(&d,layout,&poisoned);
        assert(!d.descriptor_objects && !counts.live);
    }
    ranges[0]=(VkPushConstantRange){VK_SHADER_STAGE_COMPUTE_BIT,0,16};
    ranges[1]=(VkPushConstantRange){VK_SHADER_STAGE_COMPUTE_BIT,8,16};
    assert(vkCreatePipelineLayout(&d,&info,NULL,&layout)!=VK_SUCCESS && !layout);
    ranges[1]=(VkPushConstantRange){VK_SHADER_STAGE_FRAGMENT_BIT,2,4};
    assert(vkCreatePipelineLayout(&d,&info,NULL,&layout)!=VK_SUCCESS && !layout);
    ranges[1]=(VkPushConstantRange){VK_SHADER_STAGE_FRAGMENT_BIT,252,8};
    assert(vkCreatePipelineLayout(&d,&info,NULL,&layout)!=VK_SUCCESS && !layout);
    ranges[1]=(VkPushConstantRange){VK_SHADER_STAGE_GEOMETRY_BIT,16,4};
    assert(vkCreatePipelineLayout(&d,&info,NULL,&layout)==VK_SUCCESS);
    vkDestroyPipelineLayout(&d,layout,NULL);
    ranges[1]=(VkPushConstantRange){VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT|
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,8,16};
    assert(vkCreatePipelineLayout(&d,&info,NULL,&layout)==VK_SUCCESS);
    assert(layout->push_constant_stages[2]==(VK_SHADER_STAGE_COMPUTE_BIT|
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT|VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT));
    vkDestroyPipelineLayout(&d,layout,NULL);
    ranges[1]=(VkPushConstantRange){(VkShaderStageFlags)0x80000000u,16,4};
    assert(vkCreatePipelineLayout(&d,&info,NULL,&layout)!=VK_SUCCESS && !layout);
    info.pPushConstantRanges=NULL;
    assert(vkCreatePipelineLayout(&d,&info,NULL,&layout)!=VK_SUCCESS && !layout);
    assert(!d.descriptor_objects);
}
static VkResult backing_alloc(void *ctx, VkDeviceSize size, void **address, void **backing)
{ (void)ctx; *address = malloc(size); *backing = *address; return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY; }
static void backing_free(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static void updates(void)
{
    struct VkDevice_T d = {.memory = {NULL, backing_alloc, backing_free, cache, cache},
        .buffer_alignment = 256, .noncoherent_atom = 64, .max_allocation = 4096};
    VkDescriptorSetLayout l = layout(&d), layouts[] = {l, l};
    VkDescriptorPool p = pool(&d, 2, 6, 0, NULL);
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p, .descriptorSetCount = 2, .pSetLayouts = layouts}; VkDescriptorSet s[2];
    assert(vkAllocateDescriptorSets(&d, &ai, s) == VK_SUCCESS);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 1024, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}; VkBuffer b;
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 2048};
    VkDeviceMemory m;
    assert(vkCreateBuffer(&d, &bi, NULL, &b) == VK_SUCCESS);
    assert(vkAllocateMemory(&d, &mi, NULL, &m) == VK_SUCCESS);
    assert(vkBindBufferMemory(&d, b, m, 512) == VK_SUCCESS);
    VkDescriptorBufferInfo infos[3] = {{b, 0, 256}, {b, 256, VK_WHOLE_SIZE}, {b, 512, 256}};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = s[0], .dstBinding = 1, .descriptorCount = 3,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = infos};
    VkCopyDescriptorSet c = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
        .srcSet = s[0], .srcBinding = 1, .dstSet = s[1], .dstBinding = 1, .descriptorCount = 3};
    vkUpdateDescriptorSets(&d, 1, &w, 1, &c); /* Includes skipped bindings and array rollover. */
    assert(!d.lifetime_errors && s[0]->generation == 2 && s[1]->generation == 2);
    assert(s[1]->defined[2] && s[1]->buffers[1].offset == 256 && s[1]->buffers[1].range == VK_WHOLE_SIZE);
    void *base, *span; VkDeviceSize bytes;
    assert(vkMapMemory(&d, m, 0, VK_WHOLE_SIZE, 0, &base) == VK_SUCCESS);
    assert(ps5vk_buffer_span(&d, s[1]->buffers[1].buffer, s[1]->buffers[1].offset,
                             s[1]->buffers[1].range, &span, &bytes) == VK_SUCCESS);
    assert(span == (unsigned char *)base + 768 && bytes == 768);
    infos[2].offset = 513;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);
    assert(d.lifetime_errors == 1 && s[0]->generation == 2); /* Whole write rejected. */
    infos[2].offset = 512; s[0]->pending = 1;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);
    assert(d.lifetime_errors == 2 && s[0]->generation == 2); s[0]->pending = 0;
    s[0]->defined[1] = VK_FALSE;
    vkUpdateDescriptorSets(&d, 0, NULL, 1, &c);
    assert(!s[1]->defined[1]); /* Undefined reference copy is valid, not a fake valid binding. */
    vkDestroyBuffer(&d, b, NULL);
    assert(ps5vk_buffer_span(&d, s[1]->buffers[1].buffer, 0, 1, &span, &bytes) != VK_SUCCESS);
    vkUpdateDescriptorSets(&d, 0, NULL, 1, &c); /* Copy does not dereference a destroyed resource. */
    assert(d.lifetime_errors == 2);
    vkFreeMemory(&d, m, NULL); vkDestroyDescriptorPool(&d, p, NULL);
    vkDestroyDescriptorSetLayout(&d, l, NULL);
    assert(!d.descriptor_objects && !d.buffers && !d.memories);
}
static void image_pool_types(void)
{
    struct VkDevice_T d={.graphics_enabled=VK_TRUE};
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount=2,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,.pBindings=&binding};
    VkDescriptorSetLayout layout;assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&layout)==VK_SUCCESS);
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,10};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=2,
        .poolSizeCount=1,.pPoolSizes=&size};
    VkDescriptorPool pool;assert(vkCreateDescriptorPool(&d,&pi,NULL,&pool)==VK_SUCCESS);
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};
    VkDescriptorSet set;assert(vkAllocateDescriptorSets(&d,&ai,&set)==VK_ERROR_OUT_OF_POOL_MEMORY && !set);
    vkDestroyDescriptorPool(&d,pool,NULL);size.type=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;size.descriptorCount=2;
    assert(vkCreateDescriptorPool(&d,&pi,NULL,&pool)==VK_SUCCESS);ai.descriptorPool=pool;
    assert(vkAllocateDescriptorSets(&d,&ai,&set)==VK_SUCCESS && pool->image_used==2 && !pool->storage_used);
    VkDescriptorSet extra;assert(vkAllocateDescriptorSets(&d,&ai,&extra)==VK_ERROR_OUT_OF_POOL_MEMORY);
    assert(vkResetDescriptorPool(&d,pool,0)==VK_SUCCESS && !pool->image_used);
    vkDestroyDescriptorPool(&d,pool,NULL);vkDestroyDescriptorSetLayout(&d,layout,NULL);
    assert(!d.descriptor_objects);
}
static void image_layout_visibility(void)
{
    struct VkDevice_T d={.graphics_enabled=VK_TRUE};
    VkDescriptorSetLayoutBinding binding={.binding=7,
        .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=24};
    VkDescriptorSetLayoutCreateInfo info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,.pBindings=&binding};
    const VkShaderStageFlags masks[]={VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,VK_SHADER_STAGE_ALL,
        VK_SHADER_STAGE_ALL_GRAPHICS,VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayout layout;
    for(unsigned i=0;i<sizeof(masks)/sizeof(masks[0]);++i) {
        binding.stageFlags=masks[i];
        assert(vkCreateDescriptorSetLayout(&d,&info,NULL,&layout)==VK_SUCCESS);
        assert(layout->signature.binding[7].stages==masks[i] &&
            layout->signature.binding[7].count==24 && layout->signature.count==24);
        vkDestroyDescriptorSetLayout(&d,layout,NULL);
        assert(!d.descriptor_objects);
    }
    binding.stageFlags=0;
    assert(vkCreateDescriptorSetLayout(&d,&info,NULL,&layout)!=VK_SUCCESS && !layout);
    binding.stageFlags=UINT32_C(0x40000000);
    assert(vkCreateDescriptorSetLayout(&d,&info,NULL,&layout)!=VK_SUCCESS && !layout);
    binding.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
    VkSampler immutable[24]={0};binding.pImmutableSamplers=immutable;
    assert(vkCreateDescriptorSetLayout(&d,&info,NULL,&layout)==VK_ERROR_FEATURE_NOT_PRESENT && !layout);
    binding.pImmutableSamplers=NULL;d.graphics_enabled=VK_FALSE;
    assert(vkCreateDescriptorSetLayout(&d,&info,NULL,&layout)==VK_ERROR_FEATURE_NOT_PRESENT && !layout);
    assert(!d.descriptor_objects);
}
static void uniform_resources(void)
{
    struct VkDevice_T d={.memory={NULL,backing_alloc,backing_free,cache,cache},
        .buffer_alignment=256,.uniform_buffer_alignment=256,.noncoherent_atom=64,.max_allocation=4096};
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=1024,
        .usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT};
    VkBuffer buffer;assert(vkCreateBuffer(&d,&bi,NULL,&buffer)==VK_SUCCESS);
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=1024};
    VkDeviceMemory memory;assert(vkAllocateMemory(&d,&mi,NULL,&memory)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,buffer,memory,0)==VK_SUCCESS);
    VkBufferViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,.buffer=buffer,
        .format=VK_FORMAT_R32_UINT,.offset=256,.range=256};
    VkBufferView view;assert(vkCreateBufferView(&d,&vi,NULL,&view)==VK_SUCCESS);
    VkDescriptorSetLayoutBinding bindings[]={
        {0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {1,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=2,.pBindings=bindings};VkDescriptorSetLayout layout;
    assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&layout)==VK_SUCCESS);
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1}};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};VkDescriptorPool pool;
    assert(vkCreateDescriptorPool(&d,&pi,NULL,&pool)==VK_SUCCESS);
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};VkDescriptorSet set;
    assert(vkAllocateDescriptorSets(&d,&ai,&set)==VK_SUCCESS);
    VkDescriptorBufferInfo uniform={buffer,0,256};
    VkWriteDescriptorSet writes[]={
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,
         .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&uniform},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=1,
         .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,.pTexelBufferView=&view}};
    vkUpdateDescriptorSets(&d,2,writes,0,NULL);
    assert(!d.lifetime_errors && set->defined[0] && set->defined[1] &&
        set->buffers[0].buffer==buffer && set->texel_views[1]==view);
    assert(pool->uniform_used==1 && pool->texel_used==1);
    vkDestroyDescriptorPool(&d,pool,NULL);vkDestroyDescriptorSetLayout(&d,layout,NULL);
    vkDestroyBufferView(&d,view,NULL);vkDestroyBuffer(&d,buffer,NULL);vkFreeMemory(&d,memory,NULL);
    assert(!d.buffer_views && !d.buffers && !d.memories && !d.descriptor_objects);
}
static void dynamic_buffer_resources(void)
{
    struct VkDevice_T d={.memory={NULL,backing_alloc,backing_free,cache,cache},
        .buffer_alignment=256,.uniform_buffer_alignment=256,.noncoherent_atom=64,
        .max_allocation=4096};
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=1024,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer buffer;assert(vkCreateBuffer(&d,&bi,NULL,&buffer)==VK_SUCCESS);
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=1024};
    VkDeviceMemory memory;assert(vkAllocateMemory(&d,&mi,NULL,&memory)==VK_SUCCESS);
    assert(vkBindBufferMemory(&d,buffer,memory,0)==VK_SUCCESS);
    VkDescriptorSetLayoutBinding bindings[]={
        {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=2,.pBindings=bindings};VkDescriptorSetLayout layout;
    assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&layout)==VK_SUCCESS);
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1}};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};VkDescriptorPool pool;
    assert(vkCreateDescriptorPool(&d,&pi,NULL,&pool)==VK_SUCCESS);
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};VkDescriptorSet set;
    assert(vkAllocateDescriptorSets(&d,&ai,&set)==VK_SUCCESS);
    assert(pool->dynamic_storage_used==1 && pool->dynamic_uniform_used==1 &&
        !pool->storage_used && !pool->uniform_used);
    VkDescriptorBufferInfo info={buffer,0,256};
    VkWriteDescriptorSet writes[]={
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,
         .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,.pBufferInfo=&info},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=1,
         .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.pBufferInfo=&info}};
    vkUpdateDescriptorSets(&d,2,writes,0,NULL);
    assert(!d.lifetime_errors && set->defined[0] && set->defined[1]);
    assert(vkResetDescriptorPool(&d,pool,0)==VK_SUCCESS &&
        !pool->dynamic_storage_used && !pool->dynamic_uniform_used);
    vkDestroyDescriptorPool(&d,pool,NULL);vkDestroyDescriptorSetLayout(&d,layout,NULL);
    vkDestroyBuffer(&d,buffer,NULL);vkFreeMemory(&d,memory,NULL);
}
/* Input attachments are an image-view-only descriptor role: their own pool
 * accounting, their own layout rules, and a write that stores the view and its
 * image while claiming no GPU consumption. Every fail-closed edge below leaves
 * the descriptor undefined and the set's generation untouched. */
static void input_attachments(void)
{
    struct VkDevice_T d = {.graphics_enabled=VK_TRUE}, other = {.graphics_enabled=VK_TRUE};
    struct VkImage_T image = {0}, foreign_image = {0}, unqualified_image = {0};
    image.device = &d; foreign_image.device = &other; unqualified_image.device = &d;
    /* VUID 00338: the descriptor's image must have been created for input
     * attachment use. The fixture sets exactly the bit the check requires. */
    image.info.usage = VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    foreign_image.info.usage = VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    unqualified_image.info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    struct VkImageView_T unqualified_view = {0};
    unqualified_view.device = &d; unqualified_view.image = &unqualified_image;
    unqualified_view.view_type = VK_IMAGE_VIEW_TYPE_2D;
    struct VkImageView_T view = {0}, foreign_view = {0}, no_image = {0}, wrong_type = {0};
    view.device = &d; view.image = &image; view.view_type = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    foreign_view.device = &other; foreign_view.image = &foreign_image;
    foreign_view.view_type = VK_IMAGE_VIEW_TYPE_2D;
    no_image.device = &d; no_image.view_type = VK_IMAGE_VIEW_TYPE_2D;
    wrong_type.device = &d; wrong_type.image = &image; wrong_type.view_type = VK_IMAGE_VIEW_TYPE_3D;

    /* Layout: an input attachment is an image role with no sampler, and it
     * needs the graphics backend exactly like the sampled image role. */
    VkDescriptorSetLayoutBinding binding = {.binding=3,
        .descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.descriptorCount=1,
        .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo li = {
        .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,.pBindings=&binding};
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&layout)==VK_SUCCESS && layout);
    assert(layout->signature.type[3]==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT &&
        layout->signature.binding[3].count==1);
    vkDestroyDescriptorSetLayout(&d,layout,NULL);
    /* pImmutableSamplers is meaningful only for SAMPLER and
     * COMBINED_IMAGE_SAMPLER; for an input attachment it is IGNORED, so a
     * non-null pointer is neither read nor rejected. */
    VkSampler immutable = VK_NULL_HANDLE;
    binding.pImmutableSamplers = &immutable;
    assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&layout)==VK_SUCCESS);
    vkDestroyDescriptorSetLayout(&d,layout,NULL);
    binding.pImmutableSamplers = NULL;
    binding.pImmutableSamplers = NULL;
    /* VUID 01510: an input attachment is fragment-stage only, so every other
     * visibility - including a mixed mask that contains the fragment bit - is
     * refused. */
    const VkShaderStageFlags wrong_stages[] = {VK_SHADER_STAGE_VERTEX_BIT,
        VK_SHADER_STAGE_COMPUTE_BIT,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_SHADER_STAGE_ALL};
    for (unsigned i = 0; i < sizeof(wrong_stages)/sizeof(wrong_stages[0]); ++i) {
        binding.stageFlags = wrong_stages[i];
        assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&layout)==VK_ERROR_FEATURE_NOT_PRESENT && !layout);
    }
    /* VUID 01510 permits an empty visibility mask for this descriptor type, and
     * the zero mask is accepted here without widening any other role: the
     * stored signature keeps the mask the caller asked for. */
    binding.stageFlags = 0;
    assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&layout)==VK_SUCCESS);
    assert(layout->signature.binding[3].stages == 0 &&
        layout->signature.binding[3].count == 1);
    vkDestroyDescriptorSetLayout(&d,layout,NULL);
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&layout)==VK_SUCCESS);
    vkDestroyDescriptorSetLayout(&d,layout,NULL);
    d.graphics_enabled = VK_FALSE;
    assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&layout)==VK_ERROR_FEATURE_NOT_PRESENT && !layout);
    d.graphics_enabled = VK_TRUE;
    assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&layout)==VK_SUCCESS);

    /* Pool: the role has its own accounting, so a pool sized for sampled
     * images does not hold input attachments and a pool sized for input
     * attachments does not hold sampled images. */
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
    VkDescriptorPoolCreateInfo pi = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    assert(vkCreateDescriptorPool(&d,&pi,NULL,&pool)==VK_SUCCESS);
    VkDescriptorSetAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};
    VkDescriptorSet set = VK_NULL_HANDLE;
    assert(vkAllocateDescriptorSets(&d,&ai,&set)==VK_ERROR_OUT_OF_POOL_MEMORY && !set);
    assert(!pool->image_used && !pool->input_used);
    vkDestroyDescriptorPool(&d,pool,NULL);
    size.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    assert(vkCreateDescriptorPool(&d,&pi,NULL,&pool)==VK_SUCCESS);
    /* The previous pool was destroyed, so the allocate info must name the new
     * one rather than keep pointing at freed memory. */
    ai.descriptorPool = pool;
    assert(vkAllocateDescriptorSets(&d,&ai,&set)==VK_SUCCESS && pool->input_used==1 &&
        !pool->image_used);

    /* A valid write stores the view and its image. */
    VkDescriptorImageInfo image_info = {.sampler=VK_NULL_HANDLE,.imageView=&view,
        .imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=set,.dstBinding=3,.dstArrayElement=0,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.pImageInfo=&image_info};
    vkUpdateDescriptorSets(&d,1,&write,0,NULL);
    assert(!d.lifetime_errors && set->defined[0]);
    assert(set->images[0].imageView==&view && set->image_resources[0]==&image &&
        set->images[0].sampler==VK_NULL_HANDLE);
    uint64_t generation = set->generation;
    /* GENERAL is the other layout a subpass may read through. */
    image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkUpdateDescriptorSets(&d,1,&write,0,NULL);
    assert(!d.lifetime_errors && set->defined[0] && set->images[0].imageLayout==VK_IMAGE_LAYOUT_GENERAL &&
        set->generation == generation + 1);
    generation = set->generation;

    /* Fail-closed edges: the write is refused as a whole and the previously
     * stored reference stays exactly as it was. */
    VkDescriptorImageInfo saved = set->images[0];
    VkSampler sampler = (VkSampler)(uintptr_t)0xdeadbeef;
    struct { VkDescriptorImageInfo info; unsigned expected_errors; const char *why; } cases[] = {
        {{VK_NULL_HANDLE, NULL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, 1, "no view"},
        {{VK_NULL_HANDLE, &foreign_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, 2, "foreign view"},
        {{VK_NULL_HANDLE, &no_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, 3, "view without image"},
        {{VK_NULL_HANDLE, &wrong_type, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, 4, "not a 2D view"},
        {{VK_NULL_HANDLE, &unqualified_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, 5,
         "image without the input-attachment usage"},
        {{VK_NULL_HANDLE, &view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, 6, "unreadable layout"},
        {{VK_NULL_HANDLE, &view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}, 7,
         "attachment layout is not an input layout"},
        {{VK_NULL_HANDLE, &view, VK_IMAGE_LAYOUT_UNDEFINED}, 8, "undefined layout"},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        VkDescriptorImageInfo bad_info = cases[i].info;
        VkWriteDescriptorSet bad = write; bad.pImageInfo = &bad_info;
        vkUpdateDescriptorSets(&d,1,&bad,0,NULL);
        assert(d.lifetime_errors == cases[i].expected_errors);
        assert(set->generation == generation && set->images[0].imageView == saved.imageView &&
            set->images[0].imageLayout == saved.imageLayout && set->defined[0]);
    }
    /* VkDescriptorImageInfo's sampler member is IGNORED for this descriptor
     * type, so a garbage handle is neither read nor rejected: the view and the
     * layout are what the descriptor stores. */
    VkDescriptorImageInfo ignored_sampler = {.sampler = sampler, .imageView = &view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet ignored = write; ignored.pImageInfo = &ignored_sampler;
    vkUpdateDescriptorSets(&d,1,&ignored,0,NULL);
    assert(d.lifetime_errors == 8 && set->defined[0] &&
        set->images[0].imageView == &view &&
        set->images[0].imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        /* The ignored handle is canonicalized rather than stored. */
        set->images[0].sampler == VK_NULL_HANDLE &&
        set->generation == generation + 1);
    generation = set->generation;
    /* A write covers several elements at once, and one invalid element rejects
     * the WHOLE write: element one stays exactly as it was, and the second
     * element is never published. */
    VkDescriptorSetLayoutBinding two_bindings = {.binding=3,
        .descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.descriptorCount=2,
        .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo two_info = {
        .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,.pBindings=&two_bindings};
    VkDescriptorSetLayout two_layout = VK_NULL_HANDLE;
    assert(vkCreateDescriptorSetLayout(&d,&two_info,NULL,&two_layout)==VK_SUCCESS);
    VkDescriptorPoolSize two_size = {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,2};
    VkDescriptorPoolCreateInfo two_pi = pi; two_pi.pPoolSizes = &two_size;
    VkDescriptorPool two_pool = VK_NULL_HANDLE;
    assert(vkCreateDescriptorPool(&d,&two_pi,NULL,&two_pool)==VK_SUCCESS);
    VkDescriptorSetAllocateInfo two_ai = ai; two_ai.descriptorPool = two_pool;
    two_ai.pSetLayouts = &two_layout;
    VkDescriptorSet two_set = VK_NULL_HANDLE;
    assert(vkAllocateDescriptorSets(&d,&two_ai,&two_set)==VK_SUCCESS);
    VkDescriptorImageInfo pair[2] = {
        {VK_NULL_HANDLE, &view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, &unqualified_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet pair_write = {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=two_set,.dstBinding=3,.dstArrayElement=0,.descriptorCount=2,
        .descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.pImageInfo=pair};
    vkUpdateDescriptorSets(&d,1,&pair_write,0,NULL);
    assert(d.lifetime_errors == 9);
    assert(!two_set->defined[0] && !two_set->defined[1] && two_set->generation == 1);
    /* The valid pair publishes both elements at once. */
    pair[1] = pair[0];
    vkUpdateDescriptorSets(&d,1,&pair_write,0,NULL);
    assert(d.lifetime_errors == 9 && two_set->defined[0] && two_set->defined[1] &&
        two_set->images[0].imageView == &view && two_set->images[1].imageView == &view &&
        two_set->generation == 2);
    vkDestroyDescriptorPool(&d,two_pool,NULL);
    vkDestroyDescriptorSetLayout(&d,two_layout,NULL);

    /* A write of zero descriptors is not a selection. */
    VkWriteDescriptorSet empty = write; empty.descriptorCount = 0;
    vkUpdateDescriptorSets(&d,1,&empty,0,NULL);
    assert(d.lifetime_errors == 10 && set->generation == generation);
    /* The image view must belong to this device even when the image does. */
    foreign_view.device = &d; foreign_view.image = &foreign_image;
    VkDescriptorImageInfo foreign_info = {.sampler=VK_NULL_HANDLE,.imageView=&foreign_view,
        .imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet foreign = write; foreign.pImageInfo = &foreign_info;
    vkUpdateDescriptorSets(&d,1,&foreign,0,NULL);
    assert(d.lifetime_errors == 11 && set->generation == generation);

    /* Copies move the stored reference between sets of the same type. */
    VkDescriptorPool second_pool = VK_NULL_HANDLE;
    assert(vkCreateDescriptorPool(&d,&pi,NULL,&second_pool)==VK_SUCCESS);
    VkDescriptorSet destination = VK_NULL_HANDLE;
    ai.descriptorPool = second_pool;
    assert(vkAllocateDescriptorSets(&d,&ai,&destination)==VK_SUCCESS);
    VkCopyDescriptorSet copy = {.sType=VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
        .srcSet=set,.srcBinding=3,.srcArrayElement=0,
        .dstSet=destination,.dstBinding=3,.dstArrayElement=0,.descriptorCount=1};
    vkUpdateDescriptorSets(&d,0,NULL,1,&copy);
    assert(destination->defined[0] && destination->images[0].imageView==&view &&
        destination->image_resources[0]==&image &&
        destination->images[0].sampler==VK_NULL_HANDLE && d.lifetime_errors == 11);
    /* A copy between different descriptor types is refused. */
    VkDescriptorSetLayoutBinding sampled = {.binding=3,
        .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,
        .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    li.pBindings = &sampled;
    VkDescriptorSetLayout sampled_layout = VK_NULL_HANDLE;
    assert(vkCreateDescriptorSetLayout(&d,&li,NULL,&sampled_layout)==VK_SUCCESS);
    VkDescriptorPoolSize sampled_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
    VkDescriptorPoolCreateInfo sampled_pi = pi; sampled_pi.pPoolSizes = &sampled_size;
    VkDescriptorPool sampled_pool = VK_NULL_HANDLE;
    assert(vkCreateDescriptorPool(&d,&sampled_pi,NULL,&sampled_pool)==VK_SUCCESS);
    VkDescriptorSet sampled_set = VK_NULL_HANDLE;
    ai.descriptorPool = sampled_pool; ai.pSetLayouts = &sampled_layout;
    assert(vkAllocateDescriptorSets(&d,&ai,&sampled_set)==VK_SUCCESS);
    VkCopyDescriptorSet crossed = copy; crossed.dstSet = sampled_set;
    vkUpdateDescriptorSets(&d,0,NULL,1,&crossed);
    assert(d.lifetime_errors == 12 && !sampled_set->defined[0]);

    /* Freeing returns the role's accounting. */
    assert(vkResetDescriptorPool(&d,pool,0)==VK_SUCCESS && !pool->input_used);
    vkDestroyDescriptorPool(&d,pool,NULL);
    vkDestroyDescriptorPool(&d,second_pool,NULL);
    vkDestroyDescriptorPool(&d,sampled_pool,NULL);
    vkDestroyDescriptorSetLayout(&d,sampled_layout,NULL);
    vkDestroyDescriptorSetLayout(&d,layout,NULL);
    assert(!d.descriptor_objects);
}

/* The separate sampler, sampled image and storage texel buffer types DXVK
 * 2.6.2 sizes every descriptor pool with and declares for D3D11 samplers,
 * SRVs and typed UAV buffers. Descriptor bookkeeping only: the shader-table
 * layout still has no record for them, so every consumer refuses. */
static void separate_sampler_types(void)
{
    struct VkDevice_T d = {.memory = {NULL, backing_alloc, backing_free, cache, cache},
        .buffer_alignment = 256, .noncoherent_atom = 64, .max_allocation = 4096,
        .graphics_enabled = VK_TRUE}, other = {.graphics_enabled = VK_TRUE};
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLER, 2, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings};
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    assert(vkCreateDescriptorSetLayout(&d, &li, NULL, &layout) == VK_SUCCESS);
    assert(layout->signature.count == 4 && layout->signature.binding[1].first == 2 &&
           layout->signature.type[0] == VK_DESCRIPTOR_TYPE_SAMPLER &&
           layout->signature.type[1] == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
           layout->signature.type[2] == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
    /* No shader-table record exists yet: the consumer-side layout refuses. */
    struct ps5vk_descriptor_table_layout table;
    assert(ps5vk_descriptor_table_layout_build(1, &layout->signature, &table) ==
           VK_ERROR_FEATURE_NOT_PRESENT);
    VkDescriptorSetLayout refused = VK_NULL_HANDLE;
    /* Image roles need the graphics backend; the texel role does not. */
    d.graphics_enabled = VK_FALSE; li.bindingCount = 1;
    assert(vkCreateDescriptorSetLayout(&d, &li, NULL, &refused) == VK_ERROR_FEATURE_NOT_PRESENT);
    li.pBindings = &bindings[1];
    assert(vkCreateDescriptorSetLayout(&d, &li, NULL, &refused) == VK_ERROR_FEATURE_NOT_PRESENT);
    li.pBindings = &bindings[2];
    assert(vkCreateDescriptorSetLayout(&d, &li, NULL, &refused) == VK_SUCCESS);
    vkDestroyDescriptorSetLayout(&d, refused, NULL);
    d.graphics_enabled = VK_TRUE;
    /* Immutable samplers are not implemented: refused for SAMPLER, ignored for
     * a sampled image, refused for a texel buffer. */
    VkSampler immutable = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding with_immutable = bindings[0];
    with_immutable.pImmutableSamplers = &immutable; li.pBindings = &with_immutable;
    assert(vkCreateDescriptorSetLayout(&d, &li, NULL, &refused) == VK_ERROR_FEATURE_NOT_PRESENT && !refused);
    with_immutable = bindings[1]; with_immutable.pImmutableSamplers = &immutable;
    assert(vkCreateDescriptorSetLayout(&d, &li, NULL, &refused) == VK_SUCCESS);
    vkDestroyDescriptorSetLayout(&d, refused, NULL);
    with_immutable = bindings[2]; with_immutable.pImmutableSamplers = &immutable; refused = VK_NULL_HANDLE;
    assert(vkCreateDescriptorSetLayout(&d, &li, NULL, &refused) == VK_ERROR_FEATURE_NOT_PRESENT && !refused);

    /* DXVK's pool shape (dxvk_descriptor.cpp): all eight types at once. */
    VkDescriptorPoolSize dxvk_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 32}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 128}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 64, .poolSizeCount = 8, .pPoolSizes = dxvk_sizes};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    assert(vkCreateDescriptorPool(&d, &pi, NULL, &pool) == VK_SUCCESS);
    assert(pool->sampler_capacity == 64 && pool->sampled_image_capacity == 32 &&
           pool->storage_texel_capacity == 1 && pool->image_capacity == 16);
    VkDescriptorSetLayout two[] = {layout, layout};
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 2, .pSetLayouts = two};
    VkDescriptorSet sets[2];
    /* One storage texel descriptor: the second set does not fit. */
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_ERROR_OUT_OF_POOL_MEMORY && !sets[0]);
    assert(!pool->sampler_used && !pool->sampled_image_used && !pool->storage_texel_used);
    ai.descriptorSetCount = 1;
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_SUCCESS);
    assert(pool->sampler_used == 2 && pool->sampled_image_used == 1 &&
           pool->storage_texel_used == 1 && !pool->image_used && !pool->texel_used);
    assert(vkResetDescriptorPool(&d, pool, 0) == VK_SUCCESS);
    assert(!pool->sampler_used && !pool->sampled_image_used && !pool->storage_texel_used);
    vkDestroyDescriptorPool(&d, pool, NULL);
    /* Each role has its own accounting: combined-sampler room is not sampler room. */
    VkDescriptorPoolSize only_combined = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    pi.poolSizeCount = 1; pi.pPoolSizes = &only_combined;
    assert(vkCreateDescriptorPool(&d, &pi, NULL, &pool) == VK_SUCCESS);
    ai.descriptorPool = pool;
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_ERROR_OUT_OF_POOL_MEMORY);
    vkDestroyDescriptorPool(&d, pool, NULL);
    /* Image-role pool sizes need the graphics backend. */
    d.graphics_enabled = VK_FALSE; pool = VK_NULL_HANDLE;
    VkDescriptorPoolSize sampler_size = {VK_DESCRIPTOR_TYPE_SAMPLER, 1};
    pi.pPoolSizes = &sampler_size;
    assert(vkCreateDescriptorPool(&d, &pi, NULL, &pool) == VK_ERROR_FEATURE_NOT_PRESENT && !pool);
    sampler_size.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    assert(vkCreateDescriptorPool(&d, &pi, NULL, &pool) == VK_ERROR_FEATURE_NOT_PRESENT && !pool);
    sampler_size.type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    assert(vkCreateDescriptorPool(&d, &pi, NULL, &pool) == VK_SUCCESS);
    vkDestroyDescriptorPool(&d, pool, NULL);
    d.graphics_enabled = VK_TRUE;

    pi.poolSizeCount = 8; pi.pPoolSizes = dxvk_sizes;
    assert(vkCreateDescriptorPool(&d, &pi, NULL, &pool) == VK_SUCCESS);
    ai.descriptorPool = pool; ai.descriptorSetCount = 2;
    dxvk_sizes[5].descriptorCount = 2;
    VkDescriptorPool pool2;
    assert(vkCreateDescriptorPool(&d, &pi, NULL, &pool2) == VK_SUCCESS);
    ai.descriptorPool = pool2;
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_SUCCESS);
    VkDescriptorSet set = sets[0];

    /* Resources: two samplers, a bound sampled image and texel buffer views. */
    struct VkSampler_T sampler_a = {.device = &d}, sampler_b = {.device = &d},
        foreign_sampler = {.device = &other};
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 2048};
    VkDeviceMemory memory;
    assert(vkAllocateMemory(&d, &mi, NULL, &memory) == VK_SUCCESS);
    struct VkImage_T image = {0}, unbound = {0};
    image.device = &d; image.info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    image.memory = memory; image.requirements.size = 1024;
    unbound.device = &d; unbound.info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    image.next = d.images; d.images = &image;
    struct VkImageView_T view = {0}, unbound_view = {0}, storage_only_view = {0};
    struct VkImage_T storage_only = image;
    storage_only.info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    view.device = &d; view.image = &image; view.view_type = VK_IMAGE_VIEW_TYPE_2D;
    unbound_view = view; unbound_view.image = &unbound;
    storage_only_view = view; storage_only_view.image = &storage_only;
    /* vkCreateBuffer admits STORAGE_TEXEL_BUFFER usage, but no format has a
     * storage-texel capability, so no view can satisfy a storage-texel write;
     * only the refusal is reachable. */
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 512,
        .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    VkBuffer uniform_only;
    assert(vkCreateBuffer(&d, &bi, NULL, &uniform_only) == VK_SUCCESS);
    vkDestroyBuffer(&d, uniform_only, NULL);
    bi.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    assert(vkCreateBuffer(&d, &bi, NULL, &uniform_only) == VK_SUCCESS);
    assert(vkBindBufferMemory(&d, uniform_only, memory, 1536) == VK_SUCCESS);
    VkBufferViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .buffer = uniform_only, .format = VK_FORMAT_R32_UINT, .range = 256};
    VkBufferView uniform_view;
    assert(vkCreateBufferView(&d, &vi, NULL, &uniform_view) == VK_SUCCESS);

    /* SAMPLER: imageView and imageLayout are ignored and canonicalized. */
    VkDescriptorImageInfo samplers[2] = {
        {.sampler = &sampler_a, .imageView = &view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.sampler = &sampler_b}};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
        .dstBinding = 0, .descriptorCount = 2, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo = samplers};
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);
    assert(!d.lifetime_errors && set->defined[0] && set->defined[1]);
    assert(set->images[0].sampler == &sampler_a && set->images[1].sampler == &sampler_b);
    assert(!set->images[0].imageView && set->images[0].imageLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
           !set->image_resources[0]);
    unsigned errors = 0;
    uint64_t generation = set->generation;
    samplers[1].sampler = VK_NULL_HANDLE;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);
    assert(d.lifetime_errors == ++errors);
    samplers[1].sampler = &foreign_sampler;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);
    assert(d.lifetime_errors == ++errors && set->generation == generation);
    assert(set->images[1].sampler == &sampler_b);

    /* SAMPLED_IMAGE: the combined record's image half; sampler ignored. */
    VkDescriptorImageInfo sampled = {.sampler = &foreign_sampler, .imageView = &view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstBinding = 1; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &sampled;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);
    assert(d.lifetime_errors == errors && set->defined[2]);
    assert(set->images[2].imageView == &view && !set->images[2].sampler &&
           set->image_resources[2] == &image);
    generation = set->generation;
    sampled.imageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);
    assert(d.lifetime_errors == ++errors);
    sampled.imageLayout = VK_IMAGE_LAYOUT_GENERAL; sampled.imageView = &unbound_view;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);          /* no bound memory */
    assert(d.lifetime_errors == ++errors);
    sampled.imageView = &storage_only_view;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);          /* no SAMPLED usage */
    assert(d.lifetime_errors == ++errors);
    sampled.imageView = VK_NULL_HANDLE;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);          /* null view (no nullDescriptor) */
    assert(d.lifetime_errors == ++errors && set->generation == generation);
    assert(set->images[2].imageView == &view);
    /* A combined write naming a sampled-image binding is a type mismatch. */
    sampled = (VkDescriptorImageInfo){&sampler_a, &view, VK_IMAGE_LAYOUT_GENERAL};
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);
    assert(d.lifetime_errors == ++errors);

    /* STORAGE_TEXEL_BUFFER: the view's buffer needs STORAGE_TEXEL usage. */
    w.dstBinding = 2; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    w.pImageInfo = NULL; w.pTexelBufferView = &uniform_view;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);
    assert(d.lifetime_errors == ++errors && !set->defined[3]);
    /* A uniform-texel write naming the storage-texel binding is refused. */
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    vkUpdateDescriptorSets(&d, 1, &w, 0, NULL);
    assert(d.lifetime_errors == ++errors && !set->defined[3]);

    /* Copies carry every role's payload. */
    VkCopyDescriptorSet c = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = set,
        .srcBinding = 0, .dstSet = sets[1], .dstBinding = 0, .descriptorCount = 3};
    vkUpdateDescriptorSets(&d, 0, NULL, 1, &c);
    assert(d.lifetime_errors == ++errors); /* samplers then a sampled image: one type per copy */
    c.descriptorCount = 2;
    vkUpdateDescriptorSets(&d, 0, NULL, 1, &c);
    c.srcBinding = c.dstBinding = 1; c.descriptorCount = 1;
    vkUpdateDescriptorSets(&d, 0, NULL, 1, &c);
    assert(d.lifetime_errors == errors);
    assert(sets[1]->images[1].sampler == &sampler_b && sets[1]->images[2].imageView == &view &&
           sets[1]->image_resources[2] == &image && !sets[1]->images[2].sampler);
    assert(sets[1]->defined[0] && sets[1]->defined[2] && !sets[1]->defined[3]);

    vkDestroyDescriptorPool(&d, pool, NULL); vkDestroyDescriptorPool(&d, pool2, NULL);
    vkDestroyDescriptorSetLayout(&d, layout, NULL);
    vkDestroyBufferView(&d, uniform_view, NULL); vkDestroyBuffer(&d, uniform_only, NULL);
    d.images = image.next;
    vkFreeMemory(&d, memory, NULL);
    assert(!d.descriptor_objects && !d.buffers && !d.buffer_views && !d.memories);
}

/* DXVK 2.6.2 builds one DESCRIPTOR_SET template per set layout
 * (dxvk_pipelayout.cpp): one entry per binding, descriptorCount 1,
 * offset = i * sizeof(DxvkDescriptorInfo), stride = that size, where
 * DxvkDescriptorInfo is this union. */
union dxvk_descriptor_info {
    VkDescriptorImageInfo image;
    VkDescriptorBufferInfo buffer;
    VkBufferView texelBuffer;
};
static void update_templates(void)
{
    struct counts counts = {0, -1};
    VkAllocationCallbacks callbacks = {.pUserData = &counts, .pfnAllocation = allocate,
        .pfnReallocation = reallocate, .pfnFree = release};
    struct VkDevice_T d = {.memory = {NULL, backing_alloc, backing_free, cache, cache},
        .buffer_alignment = 256, .uniform_buffer_alignment = 256, .noncoherent_atom = 64,
        .max_allocation = 4096}, other = {0};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2048,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT};
    VkBuffer buffer; assert(vkCreateBuffer(&d, &bi, NULL, &buffer) == VK_SUCCESS);
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 2048};
    VkDeviceMemory memory; assert(vkAllocateMemory(&d, &mi, NULL, &memory) == VK_SUCCESS);
    assert(vkBindBufferMemory(&d, buffer, memory, 0) == VK_SUCCESS);
    VkBufferViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .buffer = buffer, .format = VK_FORMAT_R32_UINT, .offset = 1024, .range = 256};
    VkBufferView view; assert(vkCreateBufferView(&d, &vi, NULL, &view) == VK_SUCCESS);

    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings};
    VkDescriptorSetLayout dxvk_layout, wide_layout;
    assert(vkCreateDescriptorSetLayout(&d, &li, NULL, &dxvk_layout) == VK_SUCCESS);
    li.bindingCount = 5;
    assert(vkCreateDescriptorSetLayout(&d, &li, NULL, &wide_layout) == VK_SUCCESS);

    VkDescriptorUpdateTemplateEntry entries[3];
    for (uint32_t i = 0; i < 3; ++i)
        entries[i] = (VkDescriptorUpdateTemplateEntry){.dstBinding = i, .dstArrayElement = 0,
            .descriptorCount = 1, .descriptorType = bindings[i].descriptorType,
            .offset = sizeof(union dxvk_descriptor_info) * i,
            .stride = sizeof(union dxvk_descriptor_info)};
    VkDescriptorUpdateTemplateCreateInfo ti = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .descriptorUpdateEntryCount = 3, .pDescriptorUpdateEntries = entries,
        .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
        .descriptorSetLayout = dxvk_layout};
    VkDescriptorUpdateTemplate tpl = VK_NULL_HANDLE;
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, &callbacks, &tpl) == VK_SUCCESS && tpl);
    assert(counts.live == 1 && tpl->entry_count == 3 && d.descriptor_objects == 3);
    /* The template keeps a value copy of the layout signature. */
    vkDestroyDescriptorSetLayout(&d, dxvk_layout, NULL);
    li.bindingCount = 3;
    assert(vkCreateDescriptorSetLayout(&d, &li, NULL, &dxvk_layout) == VK_SUCCESS);

    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 4}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 4, .poolSizeCount = 3, .pPoolSizes = sizes};
    VkDescriptorPool pool; assert(vkCreateDescriptorPool(&d, &pi, NULL, &pool) == VK_SUCCESS);
    VkDescriptorSetLayout set_layouts[] = {dxvk_layout, wide_layout};
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 2, .pSetLayouts = set_layouts};
    VkDescriptorSet sets[2]; assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_SUCCESS);
    VkDescriptorSet set = sets[0], wide = sets[1];

    /* pData is DXVK's descriptor array, deliberately misaligned by one byte:
     * the update copies elements and never dereferences them in place. */
    union dxvk_descriptor_info infos[3];
    memset(infos, 0, sizeof(infos));
    infos[0].buffer = (VkDescriptorBufferInfo){buffer, 0, 256};
    infos[1].buffer = (VkDescriptorBufferInfo){buffer, 512, 256};
    infos[2].texelBuffer = view;
    unsigned char raw[sizeof(infos) + 1];
    memcpy(raw + 1, infos, sizeof(infos));
    uint64_t generation = set->generation;
    vkUpdateDescriptorSetWithTemplateKHR(&d, set, tpl, raw + 1);
    assert(!d.lifetime_errors && set->generation == generation + 3);
    assert(set->defined[0] && set->defined[1] && set->defined[2]);
    assert(set->buffers[0].buffer == buffer && set->buffers[0].offset == 0 &&
           set->buffers[0].range == 256);
    assert(set->buffers[1].offset == 512 && set->texel_views[2] == view);

    /* An incompatible set (a different layout signature) is refused whole. */
    generation = wide->generation;
    vkUpdateDescriptorSetWithTemplateKHR(&d, wide, tpl, raw + 1);
    assert(d.lifetime_errors == 1 && wide->generation == generation && !wide->defined[0]);
    vkUpdateDescriptorSetWithTemplateKHR(&d, set, tpl, NULL);
    assert(d.lifetime_errors == 2);
    vkUpdateDescriptorSetWithTemplateKHR(&d, set, VK_NULL_HANDLE, raw + 1);
    assert(d.lifetime_errors == 3);
    /* An invalid entry stops the update; earlier entries stay applied. */
    infos[0].buffer.offset = 256; infos[1].buffer.offset = 3;   /* misaligned storage offset */
    infos[2].texelBuffer = view;
    generation = set->generation;
    vkUpdateDescriptorSetWithTemplateKHR(&d, set, tpl, infos);
    assert(d.lifetime_errors == 4 && set->generation == generation + 1);
    assert(set->buffers[0].offset == 256 && set->buffers[1].offset == 512);

    /* Rollover and arbitrary strides: one entry covers binding 3 (two
     * elements) and rolls into binding 4, reading every other record. */
    VkDescriptorUpdateTemplateEntry rollover = {.dstBinding = 3, .dstArrayElement = 1,
        .descriptorCount = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .offset = 8, .stride = 2 * sizeof(VkDescriptorBufferInfo)};
    ti.descriptorUpdateEntryCount = 1; ti.pDescriptorUpdateEntries = &rollover;
    ti.descriptorSetLayout = wide_layout;
    VkDescriptorUpdateTemplate wide_tpl;
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, NULL, &wide_tpl) == VK_SUCCESS);
    unsigned char packed[8 + 4 * sizeof(VkDescriptorBufferInfo)];
    memset(packed, 0xcd, sizeof(packed));
    VkDescriptorBufferInfo first = {buffer, 768, 128}, second = {buffer, 1280, 64};
    memcpy(packed + 8, &first, sizeof(first));
    memcpy(packed + 8 + 2 * sizeof(VkDescriptorBufferInfo), &second, sizeof(second));
    vkUpdateDescriptorSetWithTemplateKHR(&d, wide, wide_tpl, packed);
    const uint32_t b3 = wide->signature.binding[3].first, b4 = wide->signature.binding[4].first;
    assert(d.lifetime_errors == 4 && !wide->defined[b3] && wide->defined[b3 + 1] && wide->defined[b4]);
    assert(wide->buffers[b3 + 1].offset == 768 && wide->buffers[b4].offset == 1280 &&
           wide->buffers[b4].range == 64);

    /* Creation refusals. */
    VkDescriptorUpdateTemplate bad = VK_NULL_HANDLE;
    rollover.descriptorCount = 3;                                  /* past the last binding */
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, NULL, &bad) == VK_ERROR_UNKNOWN && !bad);
    rollover.descriptorCount = 1; rollover.dstArrayElement = 0;
    rollover.dstBinding = 2;                                       /* type differs from layout */
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, NULL, &bad) == VK_ERROR_UNKNOWN && !bad);
    rollover.dstBinding = 1; rollover.dstArrayElement = 1;         /* element past the binding */
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, NULL, &bad) == VK_ERROR_UNKNOWN && !bad);
    rollover.dstArrayElement = 0;
    rollover.dstBinding = 7;                                       /* no such binding */
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, NULL, &bad) == VK_ERROR_UNKNOWN && !bad);
    rollover.dstBinding = 3; rollover.descriptorCount = 0;
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, NULL, &bad) == VK_ERROR_UNKNOWN && !bad);
    rollover.descriptorCount = 1;
    ti.descriptorUpdateEntryCount = 0;
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, NULL, &bad) == VK_ERROR_UNKNOWN && !bad);
    ti.descriptorUpdateEntryCount = 1;
    ti.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, NULL, &bad) == VK_ERROR_FEATURE_NOT_PRESENT && !bad);
    ti.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET; ti.pNext = &ti;
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, NULL, &bad) == VK_ERROR_FEATURE_NOT_PRESENT && !bad);
    ti.pNext = NULL;
    assert(vkCreateDescriptorUpdateTemplateKHR(&other, &ti, NULL, &bad) == VK_ERROR_UNKNOWN && !bad);
    ti.descriptorSetLayout = VK_NULL_HANDLE;
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, NULL, &bad) == VK_ERROR_UNKNOWN && !bad);
    /* Allocation failure leaves nothing behind. */
    ti.descriptorSetLayout = wide_layout; counts.remaining = 0;
    assert(vkCreateDescriptorUpdateTemplateKHR(&d, &ti, &callbacks, &bad) ==
           VK_ERROR_OUT_OF_HOST_MEMORY && !bad);
    counts.remaining = -1;

    /* Destruction: foreign device ignored, then freed through the callbacks. */
    vkDestroyDescriptorUpdateTemplateKHR(&other, tpl, NULL);
    assert(counts.live == 1);
    vkDestroyDescriptorUpdateTemplateKHR(&d, tpl, &callbacks);
    vkDestroyDescriptorUpdateTemplateKHR(&d, wide_tpl, NULL);
    assert(counts.live == 0);
    vkDestroyDescriptorPool(&d, pool, NULL);
    vkDestroyDescriptorSetLayout(&d, dxvk_layout, NULL);
    vkDestroyDescriptorSetLayout(&d, wide_layout, NULL);
    vkDestroyBufferView(&d, view, NULL); vkDestroyBuffer(&d, buffer, NULL); vkFreeMemory(&d, memory, NULL);
    assert(!d.descriptor_objects && !d.buffers && !d.buffer_views && !d.memories);
}

/* DXVK262-T12: inline uniform blocks in the descriptor model only. */
static unsigned inline_invalidations;
static VkBool32 count_invalidate(VkDevice d, VkObjectType type, const void *object)
{
    (void)d; (void)object;
    assert(type == VK_OBJECT_TYPE_DESCRIPTOR_SET);
    ++inline_invalidations;
    return VK_TRUE;
}
static VkResult inline_layout(VkDevice d, const VkDescriptorSetLayoutBinding *bindings,
                              uint32_t count, VkDescriptorSetLayout *out)
{
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = count, .pBindings = bindings};
    return vkCreateDescriptorSetLayout(d, &ci, NULL, out);
}
static VkResult inline_pool(VkDevice d, uint32_t sets, uint32_t bytes, uint32_t bindings,
                            VkBool32 chain, VkDescriptorPool *out)
{
    VkDescriptorPoolInlineUniformBlockCreateInfo extra = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO,
        .maxInlineUniformBlockBindings = bindings};
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, bytes},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
    VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = chain ? &extra : NULL, .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = sets, .poolSizeCount = 2, .pPoolSizes = sizes};
    return vkCreateDescriptorPool(d, &ci, NULL, out);
}
static void inline_write_bytes(VkDevice d, VkDescriptorSet set, uint32_t binding,
                               uint32_t offset, uint32_t bytes, const void *data, uint32_t data_size)
{
    VkWriteDescriptorSetInlineUniformBlock block = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,
        .dataSize = data_size, .pData = data};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &block,
        .dstSet = set, .dstBinding = binding, .dstArrayElement = offset,
        .descriptorCount = bytes, .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK};
    vkUpdateDescriptorSets(d, 1, &w, 0, NULL);
}
static void inline_uniform_limits(void)
{
    /* Vulkan 1.3 required-limit floors, and DXVK 2.6.2's profile values. */
    assert(PS5VK_MAX_INLINE_UNIFORM_BLOCK_BYTES == 256);
    assert(PS5VK_MAX_INLINE_UNIFORM_BLOCKS_PER_STAGE == 4);
    assert(PS5VK_MAX_INLINE_UNIFORM_BLOCKS_PER_SET == 4);
    assert(PS5VK_MAX_INLINE_UNIFORM_SET_BYTES == 1024);
    assert(PS5VK_MAX_INLINE_UNIFORM_TOTAL_BYTES == 1024);
}
static void inline_uniform_layouts(void)
{
    struct VkDevice_T d = {0};
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding b[6] = {
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,
         .descriptorCount = 16, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT}};
    /* The feature is not enabled: the type is refused, never half-accepted. */
    assert(inline_layout(&d, b, 1, &l) == VK_ERROR_FEATURE_NOT_PRESENT && !l);
    d.inline_uniform_block_enabled = VK_TRUE;
    b[1] = (VkDescriptorSetLayoutBinding){.binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    b[2] = (VkDescriptorSetLayoutBinding){.binding = 5,
        .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, .descriptorCount = 256,
        .stageFlags = VK_SHADER_STAGE_ALL};
    assert(inline_layout(&d, b, 3, &l) == VK_SUCCESS);
    /* One signature slot per block; bytes live in the set's inline storage. */
    assert(l->signature.count == 4);
    assert(l->signature.binding[0].first == 0 && l->signature.binding[0].count == 2);
    assert(l->signature.binding[3].first == 2 && l->signature.binding[3].count == 1);
    assert(l->signature.binding[5].first == 3 && l->signature.binding[5].count == 1);
    assert(l->signature.type[3] == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK);
    assert(l->inline_uniform.blocks == 2 && l->inline_uniform.total_bytes == 272);
    assert(l->inline_uniform.bytes[3] == 16 && l->inline_uniform.offset[3] == 0);
    assert(l->inline_uniform.bytes[5] == 256 && l->inline_uniform.offset[5] == 16);
    assert(l->inline_uniform.bytes[0] == 0);
    /* Every consumer refuses the type explicitly: no table record exists. */
    struct ps5vk_descriptor_table_layout table;
    assert(ps5vk_descriptor_table_layout_build(1, &l->signature, &table) ==
           VK_ERROR_FEATURE_NOT_PRESENT);
    vkDestroyDescriptorSetLayout(&d, l, NULL);
    /* Zero-byte block: an empty binding that takes no slot or storage. */
    b[0].descriptorCount = 0;
    assert(inline_layout(&d, b, 1, &l) == VK_SUCCESS);
    assert(!l->signature.count && !l->signature.type[3] && !l->inline_uniform.blocks);
    vkDestroyDescriptorSetLayout(&d, l, NULL);
    /* Byte size must be a multiple of four; stages must be valid. */
    b[0].descriptorCount = 6; l = VK_NULL_HANDLE;
    assert(inline_layout(&d, b, 1, &l) == VK_ERROR_UNKNOWN && !l);
    b[0].descriptorCount = 16; b[0].stageFlags = 0;
    assert(inline_layout(&d, b, 1, &l) == VK_ERROR_UNKNOWN && !l);
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    /* maxInlineUniformBlockSize. */
    b[0].descriptorCount = 260;
    assert(inline_layout(&d, b, 1, &l) == VK_ERROR_FEATURE_NOT_PRESENT && !l);
    /* maxDescriptorSetInlineUniformBlocks: four accepted, a fifth refused. */
    for (uint32_t j = 0; j < 5; ++j)
        b[j] = (VkDescriptorSetLayoutBinding){.binding = j,
            .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, .descriptorCount = 256,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    assert(inline_layout(&d, b, 4, &l) == VK_SUCCESS);
    assert(l->inline_uniform.total_bytes == PS5VK_MAX_INLINE_UNIFORM_SET_BYTES);
    assert(l->inline_uniform.offset[3] == 768);
    vkDestroyDescriptorSetLayout(&d, l, NULL);
    assert(inline_layout(&d, b, 5, &l) == VK_ERROR_FEATURE_NOT_PRESENT && !l);
    /* Immutable samplers are ignored for an inline block, not rejected. */
    VkSampler ignored = (VkSampler)(uintptr_t)0x1234;
    b[0].pImmutableSamplers = &ignored;
    assert(inline_layout(&d, b, 1, &l) == VK_SUCCESS);
    vkDestroyDescriptorSetLayout(&d, l, NULL);
    assert(!d.descriptor_objects);
}
static void inline_uniform_pools(void)
{
    struct VkDevice_T d = {0};
    VkDescriptorPool p = VK_NULL_HANDLE;
    assert(inline_pool(&d, 1, 64, 1, VK_TRUE, &p) == VK_ERROR_FEATURE_NOT_PRESENT && !p);
    assert(inline_pool(&d, 1, 64, 1, VK_FALSE, &p) == VK_ERROR_FEATURE_NOT_PRESENT && !p);
    d.inline_uniform_block_enabled = VK_TRUE;
    /* VkDescriptorPoolSize byte counts are multiples of four. */
    assert(inline_pool(&d, 1, 66, 1, VK_TRUE, &p) == VK_ERROR_UNKNOWN && !p);
    /* An unknown pNext structure is still refused. */
    VkDescriptorPoolInlineUniformBlockCreateInfo wrong = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 64};
    VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = &wrong, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    assert(vkCreateDescriptorPool(&d, &ci, NULL, &p) == VK_ERROR_FEATURE_NOT_PRESENT && !p);

    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,
         .descriptorCount = 32, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,
         .descriptorCount = 16, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT}};
    VkDescriptorSetLayout l;
    assert(inline_layout(&d, b, 2, &l) == VK_SUCCESS);
    VkDescriptorSetLayout two[] = {l, l};
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorSetCount = 1, .pSetLayouts = two};
    VkDescriptorSet sets[2];
    /* Absent VkDescriptorPoolInlineUniformBlockCreateInfo means zero bindings. */
    assert(inline_pool(&d, 2, 96, 0, VK_FALSE, &p) == VK_SUCCESS);
    assert(p->inline_bytes_capacity == 96 && !p->inline_bindings_capacity);
    ai.descriptorPool = p;
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_ERROR_OUT_OF_POOL_MEMORY && !sets[0]);
    vkDestroyDescriptorPool(&d, p, NULL);
    /* Byte capacity: 96 bytes hold two 48-byte sets, not three. */
    assert(inline_pool(&d, 3, 96, 8, VK_TRUE, &p) == VK_SUCCESS);
    ai.descriptorPool = p; ai.descriptorSetCount = 2;
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_SUCCESS);
    assert(p->inline_bytes_used == 96 && p->inline_bindings_used == 4);
    ai.descriptorSetCount = 1;
    VkDescriptorSet extra;
    assert(vkAllocateDescriptorSets(&d, &ai, &extra) == VK_ERROR_OUT_OF_POOL_MEMORY && !extra);
    assert(vkFreeDescriptorSets(&d, p, 1, &sets[0]) == VK_SUCCESS);
    assert(p->inline_bytes_used == 48 && p->inline_bindings_used == 2);
    assert(vkAllocateDescriptorSets(&d, &ai, &extra) == VK_SUCCESS);
    assert(vkResetDescriptorPool(&d, p, 0) == VK_SUCCESS);
    assert(!p->inline_bytes_used && !p->inline_bindings_used && !p->used_sets);
    vkDestroyDescriptorPool(&d, p, NULL);
    /* Binding capacity: bytes to spare, but three bindings for four blocks. */
    assert(inline_pool(&d, 2, 1024, 3, VK_TRUE, &p) == VK_SUCCESS);
    ai.descriptorPool = p; ai.descriptorSetCount = 2;
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_ERROR_OUT_OF_POOL_MEMORY);
    assert(!sets[0] && !sets[1] && !p->inline_bindings_used);
    vkDestroyDescriptorPool(&d, p, NULL);
    vkDestroyDescriptorSetLayout(&d, l, NULL);
    assert(!d.descriptor_objects);
}
static void inline_uniform_updates(void)
{
    struct VkDevice_T d = {0};
    d.inline_uniform_block_enabled = VK_TRUE;
    d.invalidate = count_invalidate;
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,
         .descriptorCount = 32, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,
         .descriptorCount = 16, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayout l;
    assert(inline_layout(&d, b, 2, &l) == VK_SUCCESS);
    VkDescriptorPool p;
    assert(inline_pool(&d, 2, 96, 4, VK_TRUE, &p) == VK_SUCCESS);
    VkDescriptorSetLayout two[] = {l, l};
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p, .descriptorSetCount = 2, .pSetLayouts = two};
    VkDescriptorSet sets[2];
    assert(vkAllocateDescriptorSets(&d, &ai, sets) == VK_SUCCESS);
    VkDescriptorSet s = sets[0];
    const uint32_t slot2 = s->signature.binding[2].first, slot4 = s->signature.binding[4].first;
    assert(slot2 == 0 && slot4 == 1 && !s->defined[slot2]);
    uint8_t zero[48] = {0};
    assert(!memcmp(s->inline_data, zero, sizeof(zero)));

    const uint32_t words[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    uint64_t generation = s->generation;
    /* Byte offset 8, 16 bytes: lands at inline storage bytes [8, 24). */
    inline_write_bytes(&d, s, 2, 8, 16, words, 16);
    assert(!d.lifetime_errors && inline_invalidations == 1 && s->generation == generation + 1);
    assert(s->defined[slot2] && !s->defined[slot4]);
    assert(!memcmp(s->inline_data + 8, words, 16));
    assert(!memcmp(s->inline_data, zero, 8) && !memcmp(s->inline_data + 24, zero, 24));
    /* Binding 4 starts at byte 32 of the set's storage. */
    inline_write_bytes(&d, s, 4, 12, 4, &words[3], 4);
    assert(!d.lifetime_errors && s->defined[slot4]);
    assert(!memcmp(s->inline_data + 32 + 12, &words[3], 4));

    /* Rejected shapes leave contents, generation and definition untouched. */
    generation = s->generation;
    unsigned errors = 0;
    uint8_t snapshot[sizeof(s->inline_data)];
    memcpy(snapshot, s->inline_data, sizeof(snapshot));
    inline_write_bytes(&d, s, 2, 2, 4, words, 4);   /* misaligned offset */
    assert(d.lifetime_errors == ++errors);
    inline_write_bytes(&d, s, 2, 0, 6, words, 6);   /* size not a multiple of 4 */
    assert(d.lifetime_errors == ++errors);
    inline_write_bytes(&d, s, 2, 0, 8, words, 4);   /* dataSize != descriptorCount */
    assert(d.lifetime_errors == ++errors);
    inline_write_bytes(&d, s, 2, 24, 12, words, 12); /* past the block end */
    assert(d.lifetime_errors == ++errors);
    inline_write_bytes(&d, s, 2, 32, 4, words, 4);  /* offset == block size */
    assert(d.lifetime_errors == ++errors);
    inline_write_bytes(&d, s, 2, 0, 0, words, 0);   /* empty write */
    assert(d.lifetime_errors == ++errors);
    inline_write_bytes(&d, s, 3, 0, 4, words, 4);   /* not an inline binding */
    assert(d.lifetime_errors == ++errors);
    inline_write_bytes(&d, s, 2, 0, 4, NULL, 4);    /* no data */
    assert(d.lifetime_errors == ++errors);
    VkWriteDescriptorSet missing = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = s, .dstBinding = 2, .descriptorCount = 4,
        .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK};
    vkUpdateDescriptorSets(&d, 1, &missing, 0, NULL); /* no inline pNext */
    assert(d.lifetime_errors == ++errors);
    /* A buffer-typed write naming an inline binding is a type mismatch. */
    VkDescriptorBufferInfo info = {0};
    VkWriteDescriptorSet typed = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = s, .dstBinding = 2, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &info};
    vkUpdateDescriptorSets(&d, 1, &typed, 0, NULL);
    assert(d.lifetime_errors == ++errors);
    s->pending = 1;
    inline_write_bytes(&d, s, 2, 0, 4, words, 4);   /* set in flight */
    assert(d.lifetime_errors == ++errors);
    s->pending = 0;
    assert(s->generation == generation && inline_invalidations == 2);
    assert(!memcmp(snapshot, s->inline_data, sizeof(snapshot)));

    /* Copies move bytes between blocks and sets; definition follows. */
    VkDescriptorSet t = sets[1];
    VkCopyDescriptorSet c = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
        .srcSet = s, .srcBinding = 2, .srcArrayElement = 8,
        .dstSet = t, .dstBinding = 4, .dstArrayElement = 0, .descriptorCount = 16};
    vkUpdateDescriptorSets(&d, 0, NULL, 1, &c);
    assert(d.lifetime_errors == errors);
    assert(!memcmp(t->inline_data + 32, words, 16) && t->defined[slot4] && !t->defined[slot2]);
    /* An undefined source leaves the destination's definition unchanged. */
    c.srcSet = t; c.srcBinding = 2; c.srcArrayElement = 0;
    c.dstSet = s; c.dstBinding = 4; c.descriptorCount = 4;
    vkUpdateDescriptorSets(&d, 0, NULL, 1, &c);
    assert(d.lifetime_errors == errors && s->defined[slot4]);
    /* Rejected copy shapes. */
    generation = t->generation;
    c = (VkCopyDescriptorSet){.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
        .srcSet = s, .srcBinding = 2, .srcArrayElement = 2,
        .dstSet = t, .dstBinding = 2, .descriptorCount = 4};
    vkUpdateDescriptorSets(&d, 0, NULL, 1, &c);         /* misaligned source */
    assert(d.lifetime_errors == ++errors);
    c.srcArrayElement = 0; c.dstArrayElement = 30;
    vkUpdateDescriptorSets(&d, 0, NULL, 1, &c);         /* misaligned destination */
    assert(d.lifetime_errors == ++errors);
    c.dstArrayElement = 0; c.descriptorCount = 20; c.dstBinding = 4;
    vkUpdateDescriptorSets(&d, 0, NULL, 1, &c);         /* overflows 16-byte block */
    assert(d.lifetime_errors == ++errors);
    c.descriptorCount = 4; c.dstBinding = 3;
    vkUpdateDescriptorSets(&d, 0, NULL, 1, &c);         /* destination not inline */
    assert(d.lifetime_errors == ++errors);
    assert(t->generation == generation);

    vkDestroyDescriptorPool(&d, p, NULL);
    vkDestroyDescriptorSetLayout(&d, l, NULL);
    assert(!d.descriptor_objects);
}
static void inline_uniform_pipeline_layouts(void)
{
    struct VkDevice_T d = {0};
    d.inline_uniform_block_enabled = VK_TRUE;
    VkDescriptorSetLayoutBinding b[4];
    for (uint32_t j = 0; j < 4; ++j)
        b[j] = (VkDescriptorSetLayoutBinding){.binding = j,
            .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, .descriptorCount = 64,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayout frag4, vert1, all1;
    assert(inline_layout(&d, b, 4, &frag4) == VK_SUCCESS);
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    assert(inline_layout(&d, b, 1, &vert1) == VK_SUCCESS);
    b[0].stageFlags = VK_SHADER_STAGE_ALL;
    assert(inline_layout(&d, b, 1, &all1) == VK_SUCCESS);
    VkDescriptorSetLayout sets[4];
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pSetLayouts = sets};
    VkPipelineLayout pl = VK_NULL_HANDLE;
    /* Four fragment blocks plus a vertex block: 320 bytes, per stage <= 4. */
    sets[0] = frag4; sets[1] = vert1; pi.setLayoutCount = 2;
    assert(vkCreatePipelineLayout(&d, &pi, NULL, &pl) == VK_SUCCESS);
    vkDestroyPipelineLayout(&d, pl, NULL);
    /* ALL counts toward the fragment stage: five fragment blocks. */
    sets[1] = all1; pl = VK_NULL_HANDLE;
    assert(vkCreatePipelineLayout(&d, &pi, NULL, &pl) == VK_ERROR_FEATURE_NOT_PRESENT && !pl);
    /* maxInlineUniformTotalSize: four 256-byte vertex blocks meet the
     * 1024-byte total exactly (and the per-stage count). */
    for (uint32_t j = 0; j < 4; ++j) b[j].descriptorCount = 256;
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayout big, big2;
    assert(inline_layout(&d, b, 1, &big) == VK_SUCCESS);
    sets[0] = sets[1] = sets[2] = sets[3] = big; pi.setLayoutCount = 4;
    assert(vkCreatePipelineLayout(&d, &pi, NULL, &pl) == VK_SUCCESS);
    vkDestroyPipelineLayout(&d, pl, NULL);
    /* 3 x 256 vertex + 2 x 256 fragment: per-stage counts fit, 1280 bytes
     * exceed the total. */
    b[0].stageFlags = b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    assert(inline_layout(&d, b, 2, &big2) == VK_SUCCESS);
    sets[3] = big2; pl = VK_NULL_HANDLE;
    assert(vkCreatePipelineLayout(&d, &pi, NULL, &pl) == VK_ERROR_FEATURE_NOT_PRESENT && !pl);
    vkDestroyDescriptorSetLayout(&d, big2, NULL);
    vkDestroyDescriptorSetLayout(&d, big, NULL);
    vkDestroyDescriptorSetLayout(&d, all1, NULL);
    vkDestroyDescriptorSetLayout(&d, vert1, NULL);
    vkDestroyDescriptorSetLayout(&d, frag4, NULL);
    assert(!d.descriptor_objects);
}

int main(void)
{
    lifecycle(); rollback(); negative(); bda_cts_output_layout(); push_constant_layouts(); updates(); image_pool_types(); image_layout_visibility(); uniform_resources(); dynamic_buffer_resources(); input_attachments();
    separate_sampler_types();
    update_templates();
    inline_uniform_limits(); inline_uniform_layouts(); inline_uniform_pools();
    inline_uniform_updates(); inline_uniform_pipeline_layouts();
    puts("Descriptor ownership/pools/updates: pass (host only)");
}
