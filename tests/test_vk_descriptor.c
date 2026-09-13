#include "vk_descriptor.h"
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
static void push_constant_layouts(void)
{
    struct VkDevice_T d={0};
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
    ranges[1]=(VkPushConstantRange){VK_SHADER_STAGE_COMPUTE_BIT,8,16};
    assert(vkCreatePipelineLayout(&d,&info,NULL,&layout)!=VK_SUCCESS && !layout);
    ranges[1]=(VkPushConstantRange){VK_SHADER_STAGE_FRAGMENT_BIT,2,4};
    assert(vkCreatePipelineLayout(&d,&info,NULL,&layout)!=VK_SUCCESS && !layout);
    ranges[1]=(VkPushConstantRange){VK_SHADER_STAGE_FRAGMENT_BIT,252,8};
    assert(vkCreatePipelineLayout(&d,&info,NULL,&layout)!=VK_SUCCESS && !layout);
    ranges[1]=(VkPushConstantRange){VK_SHADER_STAGE_GEOMETRY_BIT,16,4};
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
int main(void)
{
    lifecycle(); rollback(); negative(); push_constant_layouts(); updates(); image_pool_types(); uniform_resources();
    puts("Descriptor ownership/pools/updates: pass (host only)");
}
