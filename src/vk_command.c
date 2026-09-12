#include "vk_command.h"
#include <string.h>

#define INVALID VK_ERROR_UNKNOWN
static void clear(VkCommandBuffer c)
{
    c->state = PS5VK_INITIAL; c->usage = 0; c->pipeline = NULL;
    c->operation_count = 0;
    c->graphics_pipeline = NULL; c->render_pass = NULL; c->framebuffer = NULL;
    memset(c->sets, 0, sizeof(c->sets)); memset(c->set_signatures, 0, sizeof(c->set_signatures));
    memset(c->graphics_sets,0,sizeof(c->graphics_sets));
    memset(c->graphics_set_signatures,0,sizeof(c->graphics_set_signatures));
    c->push_constants_valid=VK_FALSE;
    memset(c->push_constant_stages,0,sizeof(c->push_constant_stages));
    memset(c->push_constants,0,sizeof(c->push_constants));
    memset(c->operations, 0, sizeof(c->operations));
    memset(c->vertices, 0, sizeof(c->vertices));
    memset(&c->indices, 0, sizeof(c->indices));
}
static void invalid(VkCommandBuffer c)
{ if (c) { ++c->pool->device->lifetime_errors; if (c->state != PS5VK_PENDING) c->state = PS5VK_INVALID; } }
static int set_uses(VkDescriptorSet s, VkObjectType type, const void *object)
{
    if (!s) return 0;
    if (type == VK_OBJECT_TYPE_DESCRIPTOR_SET) return s == object;
    for(uint32_t j=0;j<s->signature.count;++j)if(s->defined[j]) {
        if(type==VK_OBJECT_TYPE_SAMPLER && s->images[j].sampler==object)return 1;
        if(type==VK_OBJECT_TYPE_IMAGE_VIEW && s->images[j].imageView==object)return 1;
        if(type==VK_OBJECT_TYPE_IMAGE && s->image_resources[j]==object)return 1;
        if(type==VK_OBJECT_TYPE_BUFFER_VIEW && s->texel_views[j]==object)return 1;
    }
    if (type == VK_OBJECT_TYPE_BUFFER)
        for (uint32_t j = 0; j < s->signature.count; ++j)
            if (s->defined[j] && ((const void *)s->buffers[j].buffer == object ||
                (s->texel_views[j] && (const void *)s->texel_views[j]->buffer==object))) return 1;
    return 0;
}
static int references(VkCommandBuffer c, VkObjectType type, const void *object)
{
    if (c->state == PS5VK_INITIAL || c->state == PS5VK_INVALID) return 0;
    for (uint32_t set = 0; set < PS5VK_MAX_SETS; ++set)
        if(set_uses(c->graphics_sets[set],type,object) || set_uses(c->sets[set],type,object))return 1;
    if(type==VK_OBJECT_TYPE_BUFFER && c->indices.buffer==object)return 1;
    if(type==VK_OBJECT_TYPE_BUFFER)for(unsigned k=0;k<PS5VK_MAX_VERTEX_BINDINGS;++k)
        if(c->vertices[k].buffer==object)return 1;
    if ((type == VK_OBJECT_TYPE_PIPELINE && ((const void *)c->pipeline == object ||
        (const void *)c->graphics_pipeline == object))) return 1;
    for (unsigned j = 0; j < c->operation_count; ++j)
    {
        const struct ps5vk_operation *op = &c->operations[j];
        if(type==VK_OBJECT_TYPE_BUFFER && op->buffer_barrier.buffer==object)return 1;
        if(type==VK_OBJECT_TYPE_BUFFER && op->copy_source==object)return 1;
        if(type==VK_OBJECT_TYPE_IMAGE && op->copy_image==object)return 1;
        if(type==VK_OBJECT_TYPE_IMAGE && op->image_barrier.image==object)return 1;
        if(type==VK_OBJECT_TYPE_BUFFER && op->indices.buffer==object)return 1;
        if(type==VK_OBJECT_TYPE_BUFFER)for(unsigned k=0;k<PS5VK_MAX_VERTEX_BINDINGS;++k)
            if(op->vertices[k].buffer==object)return 1;
        if ((type == VK_OBJECT_TYPE_RENDER_PASS && (const void *)op->render_pass == object) ||
            (type == VK_OBJECT_TYPE_FRAMEBUFFER && (const void *)op->framebuffer == object)) return 1;
        if (op->framebuffer) for (uint32_t k = 0; k < op->framebuffer->attachment_count; ++k) {
            VkImageView view = op->framebuffer->attachments[k];
            if ((type == VK_OBJECT_TYPE_IMAGE_VIEW && (const void *)view == object) ||
                (type == VK_OBJECT_TYPE_IMAGE && (const void *)view->image == object)) return 1;
        }
        if (type == VK_OBJECT_TYPE_PIPELINE && (const void *)c->operations[j].pipeline == object) return 1;
        for (uint32_t set = 0; set < PS5VK_MAX_SETS; ++set)
            if (set_uses(c->operations[j].sets[set], type, object)) return 1;
    }
    return 0;
}
static VkBool32 invalidate_resource(VkDevice d, VkObjectType type, const void *object)
{
    for (VkCommandPool p = d->command_pools; p; p = p->next)
        for (VkCommandBuffer c = p->buffers; c; c = c->next)
            if (c->state == PS5VK_PENDING && references(c, type, object)) return VK_FALSE;
    for (VkCommandPool p = d->command_pools; p; p = p->next)
        for (VkCommandBuffer c = p->buffers; c; c = c->next)
            if (references(c, type, object)) c->state = PS5VK_INVALID;
    return VK_TRUE;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *info,
    const VkAllocationCallbacks *a, VkCommandPool *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO || info->pNext ||
        info->queueFamilyIndex || (info->flags & ~(VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT))) return INVALID;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkCommandPool p = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, a,
        sizeof(*p), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->device = d; p->allocator = saved; p->custom_allocator = custom; p->flags = info->flags;
    p->next = d->command_pools; d->command_pools = p; d->invalidate = invalidate_resource; *out = p;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *info,
                                                       VkCommandBuffer *out)
{
    if (!info || !out) return INVALID;
    for (uint32_t j = 0; j < info->commandBufferCount; ++j) out[j] = NULL;
    if (!d || info->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO || info->pNext ||
        !info->commandPool || info->commandPool->device != d || !info->commandBufferCount ||
        info->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) return INVALID;
    VkCommandPool p = info->commandPool; VkCommandBuffer list = NULL;
    for (uint32_t j = 0; j < info->commandBufferCount; ++j) {
        VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
        VkCommandBuffer c = ps5vk_object_alloc(NULL, p->custom_allocator ? &p->allocator : NULL,
            sizeof(*c), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
        if (!c) {
            while (list) { VkCommandBuffer next = list->next; ps5vk_object_free(list, &p->allocator, p->custom_allocator); list = next; }
            for (uint32_t k = 0; k < info->commandBufferCount; ++k) out[k] = NULL;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        c->pool = p; c->next = list; list = c; out[j] = c;
    }
    while (list) { VkCommandBuffer next = list->next; list->next = p->buffers; p->buffers = list; list = next; }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice d, VkCommandPool p, uint32_t count, const VkCommandBuffer *buffers)
{
    if (!d || !p || p->device != d || (count && !buffers)) return;
    for (uint32_t j = 0; j < count; ++j) if (buffers[j]) {
        if (buffers[j]->pool != p || buffers[j]->state == PS5VK_PENDING) { ++d->lifetime_errors; return; }
        for (uint32_t k = 0; k < j; ++k) if (buffers[k] == buffers[j]) { ++d->lifetime_errors; return; }
    }
    for (uint32_t j = 0; j < count; ++j) if (buffers[j]) {
        VkCommandBuffer *link = &p->buffers;
        while (*link && *link != buffers[j]) link = &(*link)->next;
        if (*link) { *link = buffers[j]->next; ps5vk_object_free(buffers[j], &p->allocator, p->custom_allocator); }
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags flags)
{
    if (!d || !p || p->device != d || (flags & ~VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT)) return INVALID;
    for (VkCommandBuffer c = p->buffers; c; c = c->next) if (c->state == PS5VK_PENDING) return INVALID;
    for (VkCommandBuffer c = p->buffers; c; c = c->next) clear(c);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a)
{
    (void)a;
    if (!d || !p || p->device != d) return;
    if (vkResetCommandPool(d, p, 0) != VK_SUCCESS) { ++d->lifetime_errors; return; }
    while (p->buffers) { VkCommandBuffer c = p->buffers; vkFreeCommandBuffers(d, p, 1, &c); }
    VkCommandPool *link = &d->command_pools;
    while (*link && *link != p) link = &(*link)->next;
    if (*link) *link = p->next;
    VkAllocationCallbacks saved = p->allocator; VkBool32 custom = p->custom_allocator;
    ps5vk_object_free(p, &saved, custom);
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer c, VkCommandBufferResetFlags flags)
{
    if (!c || c->state == PS5VK_PENDING || !(c->pool->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) ||
        (flags & ~VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT)) return INVALID;
    clear(c); return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *info)
{
    if (!c || !info || info->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO || info->pNext ||
        c->state == PS5VK_PENDING || c->state == PS5VK_RECORDING ||
        (info->flags & ~(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                         VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)) ||
        ((info->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) &&
         (info->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT))) return INVALID;
    if (c->state != PS5VK_INITIAL && !(c->pool->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT)) return INVALID;
    clear(c); c->usage = info->flags; c->state = PS5VK_RECORDING; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c)
{
    if (!c || c->state != PS5VK_RECORDING || c->render_pass) return INVALID;
    c->state = PS5VK_EXECUTABLE; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint point, VkPipeline p)
{
    if (!c || c->state != PS5VK_RECORDING || !p || p->device != c->pool->device) { invalid(c); return; }
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS && p->graphics && c->pool->device->graphics_enabled) {
        c->graphics_pipeline = p; return;
    }
    if (point != VK_PIPELINE_BIND_POINT_COMPUTE || p->graphics) { invalid(c); return; }
    c->pipeline = p;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *offsets)
{
    (void)offsets;
    if (!c || c->state != PS5VK_RECORDING ||
        (point != VK_PIPELINE_BIND_POINT_COMPUTE && point != VK_PIPELINE_BIND_POINT_GRAPHICS) || !layout ||
        layout->device != c->pool->device || !count || !sets || dynamic_count ||
        first >= layout->set_count || count > layout->set_count - first) { invalid(c); return; }
    for (uint32_t j = 0; j < count; ++j)
        if (!sets[j] || sets[j]->pool->device != c->pool->device ||
            memcmp(&layout->sets[first+j], &sets[j]->signature, sizeof(sets[j]->signature))) {
            invalid(c); return;
        }
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS) {
        if(!c->pool->device->graphics_enabled){invalid(c);return;}
        for (uint32_t j=0;j<count;++j) { c->graphics_sets[first+j]=sets[j];
            c->graphics_set_signatures[first+j]=layout->sets[first+j]; }
    } else for (uint32_t j=0;j<count;++j) { c->sets[first+j]=sets[j];
        c->set_signatures[first+j]=layout->sets[first+j]; }
}
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer c, VkPipelineLayout layout,
    VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void *values)
{
    const VkShaderStageFlags supported = VK_SHADER_STAGE_COMPUTE_BIT |
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (!c || c->state != PS5VK_RECORDING || !layout ||
        layout->device != c->pool->device || !stages || (stages & ~supported) ||
        !size || !values || (offset & 3u) || (size & 3u) ||
        offset >= PS5VK_MAX_PUSH_CONSTANT_BYTES ||
        size > PS5VK_MAX_PUSH_CONSTANT_BYTES - offset) { invalid(c); return; }
    uint32_t first=offset/4u,end=(offset+size)/4u;
    for(uint32_t j=first;j<end;++j)
        if((layout->push_constant_stages[j]&stages)!=stages){invalid(c);return;}
    memcpy(c->push_constants+offset,values,size);
    memcpy(c->push_constant_stages,layout->push_constant_stages,
           sizeof(c->push_constant_stages));
    c->push_constants_valid=VK_TRUE;
}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z)
{
    if (!c || c->state != PS5VK_RECORDING || c->render_pass || !c->pipeline || x > 65535 || y > 65535 || z > 65535 ||
        c->operation_count == PS5VK_MAX_OPERATIONS) { invalid(c); return; }
    const struct ps5vk_compiled_program *p = &c->pipeline->program;
    if (p->push_constant_size && (!c->push_constants_valid ||
        memcmp(c->pipeline->push_constant_stages,c->push_constant_stages,
               sizeof(c->push_constant_stages)))) { invalid(c); return; }
    for (uint32_t set=0;set<c->pipeline->set_count;++set)
        if ((p->descriptor_set_mask&(1u<<set)) && (!c->sets[set] ||
            memcmp(&c->pipeline->sets[set],&c->set_signatures[set],sizeof(c->set_signatures[set]))))
            {invalid(c);return;}
    for (uint32_t j = 0; j < p->descriptor_count; ++j) {
        const struct ps5vk_program_descriptor *binding = &p->descriptors[j];
        VkDescriptorSet set=c->sets[binding->set];
        uint32_t index = set->signature.binding[binding->binding].first + binding->element;
        if (!set->defined[index]) { invalid(c); return; }
    }
    struct ps5vk_operation *op=&c->operations[c->operation_count++];
    *op=(struct ps5vk_operation){.type=PS5VK_DISPATCH,.pipeline=c->pipeline,.groups={x,y,z}};
    op->push_constant_size=p->push_constant_size;
    if(op->push_constant_size)memcpy(op->push_constants,c->push_constants,op->push_constant_size);
    for(uint32_t set=0;set<PS5VK_MAX_SETS;++set) if(p->descriptor_set_mask&(1u<<set)) {
        op->sets[set]=c->sets[set];op->generations[set]=c->sets[set]->generation;
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo *info,
    VkSubpassContents contents)
{
    if (!c || c->state != PS5VK_RECORDING || !c->pool->device->graphics_enabled || c->render_pass ||
        !info || info->sType != VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO || info->pNext ||
        contents != VK_SUBPASS_CONTENTS_INLINE || !info->renderPass || !info->framebuffer ||
        info->renderPass->device != c->pool->device || info->framebuffer->device != c->pool->device ||
        info->clearValueCount > 2 || (info->clearValueCount && !info->pClearValues) ||
        c->operation_count == PS5VK_MAX_OPERATIONS) { invalid(c); return; }
    VkRenderPass pass = info->renderPass; VkFramebuffer fb = info->framebuffer;
    VkRect2D area = info->renderArea;
    if (fb->attachment_count != pass->attachment_count || fb->color_attachment != pass->color.attachment ||
        fb->depth_attachment != pass->depth.attachment || area.offset.x < 0 || area.offset.y < 0 ||
        !area.extent.width || !area.extent.height || (uint32_t)area.offset.x > fb->width ||
        (uint32_t)area.offset.y > fb->height || area.extent.width > fb->width - (uint32_t)area.offset.x ||
        area.extent.height > fb->height - (uint32_t)area.offset.y) { invalid(c); return; }
    for (uint32_t j = 0; j < pass->attachment_count; ++j) {
        const VkAttachmentDescription *a = &pass->attachments[j];
        if (fb->formats[j] != a->format || fb->samples[j] != a->samples ||
            (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && info->clearValueCount <= j)) { invalid(c); return; }
    }
    struct ps5vk_operation *op = &c->operations[c->operation_count++];
    *op = (struct ps5vk_operation){.type = PS5VK_BEGIN_RENDER_PASS, .render_pass = pass,
        .framebuffer = fb, .render_area = area, .clear_count = info->clearValueCount};
    if (info->clearValueCount) memcpy(op->clears, info->pClearValues, info->clearValueCount * sizeof(VkClearValue));
    c->render_pass = pass; c->framebuffer = fb;
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer c)
{
    if (!c || c->state != PS5VK_RECORDING || !c->render_pass ||
        c->operation_count == PS5VK_MAX_OPERATIONS) { invalid(c); return; }
    c->operations[c->operation_count++] = (struct ps5vk_operation){.type = PS5VK_END_RENDER_PASS,
        .render_pass = c->render_pass, .framebuffer = c->framebuffer};
    c->render_pass = NULL; c->framebuffer = NULL;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer c,uint32_t first,uint32_t count,
    const VkBuffer *buffers,const VkDeviceSize *offsets)
{
    if(!c || c->state!=PS5VK_RECORDING || first>=PS5VK_MAX_VERTEX_BINDINGS ||
       count>PS5VK_MAX_VERTEX_BINDINGS-first || (count && (!buffers || !offsets))) {invalid(c);return;}
    for(uint32_t i=0;i<count;++i) {
        void *address;VkDeviceSize bytes;
        if(!ps5vk_buffer_usage(c->pool->device,buffers[i],VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
           ps5vk_buffer_span(c->pool->device,buffers[i],offsets[i],VK_WHOLE_SIZE,&address,&bytes)!=VK_SUCCESS)
            {invalid(c);return;}
    }
    for(uint32_t i=0;i<count;++i)c->vertices[first+i]=(struct ps5vk_vertex_binding){buffers[i],offsets[i]};
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer c,VkBuffer buffer,
    VkDeviceSize offset,VkIndexType type)
{
    if(!c || c->state!=PS5VK_RECORDING ||
       (type!=VK_INDEX_TYPE_UINT16 && type!=VK_INDEX_TYPE_UINT32)) {invalid(c);return;}
    void *address;VkDeviceSize bytes;
    const unsigned size=type==VK_INDEX_TYPE_UINT16?2:4;
    if(!ps5vk_buffer_usage(c->pool->device,buffer,VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
       ps5vk_buffer_span(c->pool->device,buffer,offset,VK_WHOLE_SIZE,&address,&bytes)!=VK_SUCCESS ||
       (uintptr_t)address%size) {invalid(c);return;}
    c->indices=(struct ps5vk_index_binding){buffer,offset,type};
}
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer c, uint32_t vertices, uint32_t instances,
    uint32_t first_vertex, uint32_t first_instance)
{
    if (!c || c->state != PS5VK_RECORDING || !c->render_pass || !c->graphics_pipeline ||
        c->operation_count == PS5VK_MAX_OPERATIONS) { invalid(c); return; }
    VkPipeline p = c->graphics_pipeline;
    if(p->push_constant_size && (!c->push_constants_valid ||
        memcmp(p->push_constant_stages,c->push_constant_stages,
               sizeof(c->push_constant_stages)))) {invalid(c);return;}
    if(p->set_count && (p->set_count!=1 || !c->graphics_sets[0] ||
        memcmp(&p->sets[0],&c->graphics_set_signatures[0],sizeof(p->sets[0])))) {invalid(c);return;}
    VkRenderPass pass = c->render_pass;
    VkFormat depth = pass->depth.attachment == VK_ATTACHMENT_UNUSED ? VK_FORMAT_UNDEFINED :
        pass->attachments[pass->depth.attachment].format;
    if (p->color_format != pass->attachments[pass->color.attachment].format || p->depth_format != depth) {
        invalid(c); return;
    }
    c->operations[c->operation_count++] = (struct ps5vk_operation){.type = PS5VK_DRAW,
        .pipeline = p, .render_pass = pass, .framebuffer = c->framebuffer,
        .vertex_count = vertices, .instance_count = instances, .first_vertex = first_vertex,
        .first_instance = first_instance};
    memcpy(c->operations[c->operation_count-1].vertices,c->vertices,sizeof(c->vertices));
    c->operations[c->operation_count-1].push_constant_size=p->push_constant_size;
    if(p->push_constant_size)memcpy(c->operations[c->operation_count-1].push_constants,
        c->push_constants,p->push_constant_size);
    if(p->set_count) {
        c->operations[c->operation_count-1].sets[0]=c->graphics_sets[0];
        c->operations[c->operation_count-1].generations[0]=c->graphics_sets[0]->generation;
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer c,uint32_t count,uint32_t instances,
    uint32_t first,int32_t base,uint32_t first_instance)
{
    if(!c || c->state!=PS5VK_RECORDING || !c->indices.buffer) {invalid(c);return;}
    void *address;VkDeviceSize bytes;
    const unsigned size=c->indices.type==VK_INDEX_TYPE_UINT16?2:4;
    if(ps5vk_buffer_span(c->pool->device,c->indices.buffer,c->indices.offset,VK_WHOLE_SIZE,
        &address,&bytes)!=VK_SUCCESS || (uint64_t)first+count>bytes/size) {invalid(c);return;}
    /* Reuse render-pass/pipeline compatibility checks, then replace the record's
     * kind. Never encode signed baseVertex into non-indexed firstVertex. */
    vkCmdDraw(c,0,instances,0,first_instance);
    if(c->state!=PS5VK_RECORDING)return;
    struct ps5vk_operation *op=&c->operations[c->operation_count-1];
    op->type=PS5VK_DRAW_INDEXED;op->index_count=count;op->first_index=first;
    op->vertex_offset=base;op->indices=c->indices;
}
static int texture_layout_supported(VkImageLayout layout)
{
    return layout==VK_IMAGE_LAYOUT_GENERAL || layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}
static int texture_scope(VkPipelineStageFlags stages, VkAccessFlags access)
{
    const VkPipelineStageFlags allowed=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if(!stages || (stages & ~allowed) ||
        (access & ~(VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)))return 0;
    if(stages & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)return 1;
    if((access & VK_ACCESS_TRANSFER_WRITE_BIT) && !(stages & VK_PIPELINE_STAGE_TRANSFER_BIT))return 0;
    if((access & VK_ACCESS_SHADER_READ_BIT) && !(stages & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT))return 0;
    return 1;
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count,
    const VkBufferMemoryBarrier *buffers, uint32_t image_count, const VkImageMemoryBarrier *images)
{
    if(image_count) {
        if(!c || c->state!=PS5VK_RECORDING || c->render_pass || flags || memory_count || buffer_count ||
            !images || !c->pool->device->graphics_enabled ||
            image_count>PS5VK_MAX_OPERATIONS-c->operation_count) {invalid(c);return;}
        /* Validate the whole batch before appending. Recording does not execute
         * transitions: old-layout reconciliation belongs to queue execution. */
        for(uint32_t j=0;j<image_count;++j) {
            const VkImageMemoryBarrier *b=&images[j];
            VkImage image=b->image;void *address;VkDeviceSize bytes;
            if(b->sType!=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER || b->pNext ||
                !texture_scope(src,b->srcAccessMask) || !texture_scope(dst,b->dstAccessMask) ||
                (!texture_layout_supported(b->oldLayout) && b->oldLayout!=VK_IMAGE_LAYOUT_UNDEFINED) ||
                !texture_layout_supported(b->newLayout) ||
                b->srcQueueFamilyIndex!=b->dstQueueFamilyIndex ||
                (b->srcQueueFamilyIndex!=0 && b->srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED) ||
                !image || image->device!=c->pool->device || image->info.format!=VK_FORMAT_R8G8B8A8_UNORM ||
                image->info.mipLevels!=1 || image->info.arrayLayers!=1 ||
                (image->info.usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))!=
                    (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
                (image->info.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) ||
                b->subresourceRange.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT ||
                b->subresourceRange.baseMipLevel || b->subresourceRange.baseArrayLayer ||
                (b->subresourceRange.levelCount!=1 && b->subresourceRange.levelCount!=VK_REMAINING_MIP_LEVELS) ||
                (b->subresourceRange.layerCount!=1 && b->subresourceRange.layerCount!=VK_REMAINING_ARRAY_LAYERS) ||
                ps5vk_image_span(c->pool->device,image,&address,&bytes)!=VK_SUCCESS) {invalid(c);return;}
        }
        for(uint32_t j=0;j<image_count;++j)
            c->operations[c->operation_count++]=(struct ps5vk_operation){.type=PS5VK_IMAGE_BARRIER,
                .image_barrier=images[j],.src_stage=src,.dst_stage=dst,
                .src_access=images[j].srcAccessMask,.dst_access=images[j].dstAccessMask};
        return;
    }
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT |
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkAccessFlags accesses = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT |
        VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    if (!c || c->state != PS5VK_RECORDING || c->render_pass || !src || !dst || (src & ~stages) || (dst & ~stages) || flags ||
        image_count || (memory_count && !memory) || (buffer_count && !buffers) ||
        c->operation_count == PS5VK_MAX_OPERATIONS ||
        buffer_count > PS5VK_MAX_OPERATIONS - c->operation_count - 1) { invalid(c); return; }
    VkAccessFlags src_access = 0, dst_access = 0;
    for (uint32_t j = 0; j < memory_count; ++j) {
        if (memory[j].sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER || memory[j].pNext ||
            ((memory[j].srcAccessMask | memory[j].dstAccessMask) & ~accesses)) { invalid(c); return; }
        src_access |= memory[j].srcAccessMask; dst_access |= memory[j].dstAccessMask;
    }
    /* A full cache dependency is stronger than a buffer-range dependency.
     * Retain the buffer/range for lifetime validation; never ignore ownership
     * transfers or accept a range outside its bound allocation. */
    for (uint32_t j = 0; j < buffer_count; ++j) {
        const VkBufferMemoryBarrier *b = &buffers[j];
        void *address; VkDeviceSize bytes;
        if (b->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER || b->pNext ||
            ((b->srcAccessMask | b->dstAccessMask) & ~accesses) ||
            b->srcQueueFamilyIndex != b->dstQueueFamilyIndex ||
            (b->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED && b->srcQueueFamilyIndex != 0) ||
            ps5vk_buffer_span(c->pool->device, b->buffer, b->offset, b->size,
                &address, &bytes) != VK_SUCCESS) { invalid(c); return; }
    }
    for (uint32_t j = 0; j < buffer_count; ++j)
        c->operations[c->operation_count++] = (struct ps5vk_operation){.type = PS5VK_BARRIER,
            .src_stage = src, .dst_stage = dst, .src_access = buffers[j].srcAccessMask,
            .dst_access = buffers[j].dstAccessMask, .buffer_barrier = buffers[j]};
    c->operations[c->operation_count++] = (struct ps5vk_operation){.type = PS5VK_BARRIER,
        .src_stage = src, .dst_stage = dst, .src_access = src_access, .dst_access = dst_access};
}
