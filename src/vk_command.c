#include "vk_command.h"
#include "vk_query_pool.h"
#include "vk_image.h"
#include "vk_image_transfer.h"
#include "vk_sync.h"
#include "color_barrier.h"
#include <float.h>
#include <string.h>

#define INVALID VK_ERROR_UNKNOWN
enum {
    PS5VK_DYNAMIC_LINE_WIDTH = 1u << 0,
    PS5VK_DYNAMIC_DEPTH_BIAS = 1u << 1,
    PS5VK_DYNAMIC_BLEND_CONSTANTS = 1u << 2,
    PS5VK_DYNAMIC_DEPTH_BOUNDS = 1u << 3,
    PS5VK_DYNAMIC_STENCIL_COMPARE_MASK = 1u << 4,
    PS5VK_DYNAMIC_STENCIL_WRITE_MASK = 1u << 5,
    PS5VK_DYNAMIC_STENCIL_REFERENCE = 1u << 6,
};
static void clear(VkCommandBuffer c)
{
    for (unsigned j = 0; j < c->operation_count; ++j) {
        struct ps5vk_operation *op = &c->operations[j];
        if (op->owned_payload)
            ps5vk_object_free(op->owned_payload, &op->payload_allocator,
                op->custom_payload_allocator);
    }
    c->state = PS5VK_INITIAL; c->usage = 0; c->pipeline = NULL;
    c->operation_count = 0;
    c->graphics_pipeline = NULL; c->render_pass = NULL; c->framebuffer = NULL;
    c->viewport_valid = c->scissor_valid = VK_FALSE;
    c->line_width = 1.0f;
    c->min_depth_bounds = 0.0f;
    c->max_depth_bounds = 1.0f;
    c->dynamic_state_valid = 0;
    memset(c->blend_constants, 0, sizeof(c->blend_constants));
    memset(c->stencil_compare_mask, 0, sizeof(c->stencil_compare_mask));
    memset(c->stencil_write_mask, 0, sizeof(c->stencil_write_mask));
    memset(c->stencil_reference, 0, sizeof(c->stencil_reference));
    c->stencil_compare_faces = c->stencil_write_faces =
        c->stencil_reference_faces = 0;
    memset(c->sets, 0, sizeof(c->sets)); memset(c->set_signatures, 0, sizeof(c->set_signatures));
    memset(c->set_dynamic_offsets,0,sizeof(c->set_dynamic_offsets));
    memset(c->graphics_sets,0,sizeof(c->graphics_sets));
    memset(c->graphics_set_signatures,0,sizeof(c->graphics_set_signatures));
    memset(c->graphics_set_dynamic_offsets,0,sizeof(c->graphics_set_dynamic_offsets));
    c->push_constants_valid=VK_FALSE;
    memset(c->push_constant_stages,0,sizeof(c->push_constant_stages));
    memset(c->push_constants,0,sizeof(c->push_constants));
    memset(c->operations, 0, sizeof(c->operations));
    memset(c->vertices, 0, sizeof(c->vertices));
    memset(&c->indices, 0, sizeof(c->indices));
}
void ps5vk_command_invalidate(VkCommandBuffer c)
{
    if (c) { ++c->pool->device->lifetime_errors; if (c->state != PS5VK_PENDING) c->state = PS5VK_INVALID; }
}
#define invalid ps5vk_command_invalidate

struct ps5vk_operation *ps5vk_command_reserve_operations(VkCommandBuffer c,
    enum ps5vk_operation_type type, enum ps5vk_operation_scope scope, uint32_t count)
{
    if (!c || c->state != PS5VK_RECORDING || !count ||
        scope < PS5VK_OPERATION_OUTSIDE_RENDER_PASS ||
        scope > PS5VK_OPERATION_ANYWHERE ||
        (scope == PS5VK_OPERATION_OUTSIDE_RENDER_PASS && c->render_pass) ||
        (scope == PS5VK_OPERATION_INSIDE_RENDER_PASS && !c->render_pass) ||
        c->operation_count > PS5VK_MAX_OPERATIONS ||
        count > PS5VK_MAX_OPERATIONS - c->operation_count) {
        invalid(c);
        return NULL;
    }
    struct ps5vk_operation *first = &c->operations[c->operation_count];
    memset(first, 0, count * sizeof(*first));
    for (uint32_t j = 0; j < count; ++j) first[j].type = type;
    c->operation_count += count;
    return first;
}

struct ps5vk_operation *ps5vk_command_reserve_operation_with_payload(VkCommandBuffer c,
    enum ps5vk_operation_type type, enum ps5vk_operation_scope scope,
    const void *data, size_t size)
{
    if (!c || !data || !size) {
        invalid(c);
        return NULL;
    }
    VkAllocationCallbacks saved = {0};
    VkBool32 custom = VK_FALSE;
    void *payload = ps5vk_object_alloc(NULL,
        c->pool->custom_allocator ? &c->pool->allocator : NULL,
        size, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!payload) {
        invalid(c);
        return NULL;
    }
    memcpy(payload, data, size);
    struct ps5vk_operation *op = ps5vk_command_reserve_operations(c, type, scope, 1);
    if (!op) {
        ps5vk_object_free(payload, &saved, custom);
        return NULL;
    }
    op->owned_payload = payload;
    op->owned_payload_size = size;
    op->payload_allocator = saved;
    op->custom_payload_allocator = custom;
    return op;
}
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
        if (type == VK_OBJECT_TYPE_EVENT && (const void *)op->event == object) return 1;
        if (type == VK_OBJECT_TYPE_QUERY_POOL &&
            (const void *)op->query_pool == object) return 1;
        if(type==VK_OBJECT_TYPE_BUFFER && op->buffer_barrier.buffer==object)return 1;
        if(type==VK_OBJECT_TYPE_BUFFER && op->copy_source==object)return 1;
        if(type==VK_OBJECT_TYPE_BUFFER && op->copy_destination==object)return 1;
        if(type==VK_OBJECT_TYPE_BUFFER && op->indirect_buffer==object)return 1;
        if(type==VK_OBJECT_TYPE_IMAGE && op->copy_image==object)return 1;
        if(type==VK_OBJECT_TYPE_IMAGE && op->image_source==object)return 1;
        if(type==VK_OBJECT_TYPE_IMAGE && op->image_destination==object)return 1;
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
        if (*link) {
            *link = buffers[j]->next;
            clear(buffers[j]);
            ps5vk_object_free(buffers[j], &p->allocator, p->custom_allocator);
        }
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
    clear(c); c->usage = info->flags; c->state = PS5VK_RECORDING;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c)
{
    if (!c || c->state != PS5VK_RECORDING || c->render_pass) return INVALID;
    c->state = PS5VK_EXECUTABLE;
    return VK_SUCCESS;
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
static int valid_viewport(const VkViewport *v)
{
    return v && v->x >= -FLT_MAX && v->x <= FLT_MAX &&
        v->y >= -FLT_MAX && v->y <= FLT_MAX &&
        v->width > 0 && v->width <= FLT_MAX && v->height > 0 && v->height <= FLT_MAX &&
        v->minDepth >= 0 && v->minDepth <= 1 && v->maxDepth >= 0 && v->maxDepth <= 1;
}
static int valid_scissor(const VkRect2D *s)
{
    return s && s->offset.x >= 0 && s->offset.y >= 0 &&
        s->extent.width && s->extent.height &&
        (uint64_t)(uint32_t)s->offset.x + s->extent.width <= UINT32_MAX &&
        (uint64_t)(uint32_t)s->offset.y + s->extent.height <= UINT32_MAX;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer c, uint32_t first,
    uint32_t count, const VkViewport *viewports)
{
    if (!c || c->state != PS5VK_RECORDING || first || count != 1 ||
        !valid_viewport(viewports)) { invalid(c); return; }
    c->viewport = viewports[0]; c->viewport_valid = VK_TRUE;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer c, uint32_t first,
    uint32_t count, const VkRect2D *scissors)
{
    if (!c || c->state != PS5VK_RECORDING || first || count != 1 ||
        !valid_scissor(scissors)) { invalid(c); return; }
    c->scissor = scissors[0]; c->scissor_valid = VK_TRUE;
}
static int recording(VkCommandBuffer c)
{
    if (!c || c->state != PS5VK_RECORDING) { invalid(c); return 0; }
    return 1;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer c, float width)
{
    /* wideLines is not advertised, so Vulkan 1.0 permits only 1.0f. */
    if (!recording(c)) return;
    if (width != 1.0f) { invalid(c); return; }
    c->line_width = width;
    c->dynamic_state_valid |= PS5VK_DYNAMIC_LINE_WIDTH;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer c, float constant,
    float clamp, float slope)
{
    if (!recording(c)) return;
    /* depthBiasClamp is not advertised, so its valid value is exactly zero.
     * Vulkan places no finiteness restriction on the other two factors; retain
     * their float bit patterns without inventing a narrower API contract. */
    if (clamp != 0.0f) {
        invalid(c); return;
    }
    c->depth_bias_constant=constant;c->depth_bias_clamp=clamp;c->depth_bias_slope=slope;
    c->dynamic_state_valid |= PS5VK_DYNAMIC_DEPTH_BIAS;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer c, const float values[4])
{
    if (!recording(c)) return;
    if (!values) { invalid(c); return; }
    memcpy(c->blend_constants, values, sizeof(c->blend_constants));
    c->dynamic_state_valid |= PS5VK_DYNAMIC_BLEND_CONSTANTS;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer c, float minimum, float maximum)
{
    if (!recording(c)) return;
    /* Positive-form comparisons also reject NaN. */
    if (!(minimum >= 0.0f && maximum <= 1.0f && minimum <= maximum)) {
        invalid(c); return;
    }
    c->min_depth_bounds=minimum;c->max_depth_bounds=maximum;
    c->dynamic_state_valid |= PS5VK_DYNAMIC_DEPTH_BOUNDS;
}
static int stencil_faces(VkCommandBuffer c, VkStencilFaceFlags faces, uint32_t value,
    uint32_t state[2], VkStencilFaceFlags *initialized, uint32_t bit)
{
    const VkStencilFaceFlags supported=VK_STENCIL_FACE_FRONT_BIT|VK_STENCIL_FACE_BACK_BIT;
    if (!recording(c)) return 0;
    if (!faces || (faces & ~supported)) { invalid(c); return 0; }
    if (faces & VK_STENCIL_FACE_FRONT_BIT) state[0]=value;
    if (faces & VK_STENCIL_FACE_BACK_BIT) state[1]=value;
    *initialized |= faces;
    c->dynamic_state_valid |= bit;
    return 1;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer c,
    VkStencilFaceFlags faces, uint32_t mask)
{
    (void)stencil_faces(c,faces,mask,c ? c->stencil_compare_mask : NULL,
        c ? &c->stencil_compare_faces : NULL,
        PS5VK_DYNAMIC_STENCIL_COMPARE_MASK);
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer c,
    VkStencilFaceFlags faces, uint32_t mask)
{
    (void)stencil_faces(c,faces,mask,c ? c->stencil_write_mask : NULL,
        c ? &c->stencil_write_faces : NULL,
        PS5VK_DYNAMIC_STENCIL_WRITE_MASK);
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer c,
    VkStencilFaceFlags faces, uint32_t reference)
{
    (void)stencil_faces(c,faces,reference,c ? c->stencil_reference : NULL,
        c ? &c->stencil_reference_faces : NULL,
        PS5VK_DYNAMIC_STENCIL_REFERENCE);
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *offsets)
{
    if (!c || c->state != PS5VK_RECORDING ||
        (point != VK_PIPELINE_BIND_POINT_COMPUTE && point != VK_PIPELINE_BIND_POINT_GRAPHICS) || !layout ||
        layout->device != c->pool->device || !count || !sets || (dynamic_count && !offsets) ||
        first >= layout->set_count || count > layout->set_count - first) { invalid(c); return; }
    uint32_t expected_dynamic=0;
    for (uint32_t j = 0; j < count; ++j)
        if (!sets[j] || sets[j]->pool->device != c->pool->device ||
            memcmp(&layout->sets[first+j], &sets[j]->signature, sizeof(sets[j]->signature))) {
            invalid(c); return;
        } else for(uint32_t binding=0;binding<PS5VK_MAX_BINDINGS;++binding)
            if(ps5vk_dynamic_descriptor_type(sets[j]->signature.type[binding]))
                expected_dynamic+=sets[j]->signature.binding[binding].count;
    if(dynamic_count!=expected_dynamic){invalid(c);return;}
    VkDeviceSize prepared[PS5VK_MAX_SETS][PS5VK_MAX_DESCRIPTORS]={{0}};
    uint32_t cursor=0;
    for(uint32_t j=0;j<count;++j) {
        VkDescriptorSet set=sets[j];
        for(uint32_t binding=0;binding<PS5VK_MAX_BINDINGS;++binding) {
            const struct ps5vk_binding *b=&set->signature.binding[binding];
            if(!ps5vk_dynamic_descriptor_type(set->signature.type[binding]))continue;
            VkDeviceSize alignment=ps5vk_base_buffer_descriptor_type(set->signature.type[binding])==
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?c->pool->device->uniform_buffer_alignment:
                c->pool->device->buffer_alignment;
            for(uint32_t element=0;element<b->count;++element) {
                VkDeviceSize offset=offsets[cursor++];
                if(!alignment || offset%alignment){invalid(c);return;}
                prepared[j][b->first+element]=offset;
            }
        }
    }
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS) {
        if(!c->pool->device->graphics_enabled){invalid(c);return;}
        for (uint32_t j=0;j<count;++j) { c->graphics_sets[first+j]=sets[j];
            c->graphics_set_signatures[first+j]=layout->sets[first+j];
            memcpy(c->graphics_set_dynamic_offsets[first+j],prepared[j],sizeof(prepared[j])); }
    } else for (uint32_t j=0;j<count;++j) { c->sets[first+j]=sets[j];
        c->set_signatures[first+j]=layout->sets[first+j];
        memcpy(c->set_dynamic_offsets[first+j],prepared[j],sizeof(prepared[j])); }
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
    struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,PS5VK_DISPATCH,
        PS5VK_OPERATION_OUTSIDE_RENDER_PASS,1);
    if(!op)return;
    op->pipeline=c->pipeline;op->groups[0]=x;op->groups[1]=y;op->groups[2]=z;
    op->push_constant_size=p->push_constant_size;
    if(op->push_constant_size)memcpy(op->push_constants,c->push_constants,op->push_constant_size);
    for(uint32_t set=0;set<PS5VK_MAX_SETS;++set) if(p->descriptor_set_mask&(1u<<set)) {
        op->sets[set]=c->sets[set];op->generations[set]=c->sets[set]->generation;
    }
    for(uint32_t j=0;j<p->descriptor_count;++j) {
        const struct ps5vk_program_descriptor *binding=&p->descriptors[j];
        uint32_t index=c->sets[binding->set]->signature.binding[binding->binding].first+
            binding->element;
        op->descriptor_dynamic_offsets[j]=c->set_dynamic_offsets[binding->set][index];
    }
}
static VkBool32 indirect_buffer_valid(VkCommandBuffer c, VkBuffer buffer,
    VkDeviceSize offset, VkDeviceSize bytes)
{
    void *address = NULL; VkDeviceSize available = 0;
    return c && c->state == PS5VK_RECORDING && !(offset & 3u) &&
        ps5vk_buffer_usage(c->pool->device, buffer, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) &&
        ps5vk_buffer_span(c->pool->device, buffer, offset, bytes,
            &address, &available) == VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer c,VkBuffer buffer,
    VkDeviceSize offset)
{
    if(!indirect_buffer_valid(c,buffer,offset,sizeof(VkDispatchIndirectCommand)))
        {invalid(c);return;}
    vkCmdDispatch(c,0,0,0);if(!c || c->state!=PS5VK_RECORDING)return;
    struct ps5vk_operation *op=&c->operations[c->operation_count-1];
    op->type=PS5VK_DISPATCH_INDIRECT;op->indirect_buffer=buffer;
    op->indirect_offset=offset;op->indirect_count=1;
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
    struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,PS5VK_BEGIN_RENDER_PASS,
        PS5VK_OPERATION_OUTSIDE_RENDER_PASS,1);
    if(!op)return;
    op->render_pass=pass;op->framebuffer=fb;op->render_area=area;
    op->clear_count=info->clearValueCount;
    if (info->clearValueCount) memcpy(op->clears, info->pClearValues, info->clearValueCount * sizeof(VkClearValue));
    c->render_pass = pass; c->framebuffer = fb;
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer c)
{
    if (!c || c->state != PS5VK_RECORDING || !c->render_pass ||
        c->operation_count == PS5VK_MAX_OPERATIONS) { invalid(c); return; }
    struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,PS5VK_END_RENDER_PASS,
        PS5VK_OPERATION_INSIDE_RENDER_PASS,1);
    if(!op)return;
    op->render_pass=c->render_pass;op->framebuffer=c->framebuffer;
    c->render_pass = NULL; c->framebuffer = NULL;
}
VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer c,
    VkSubpassContents contents)
{
    /* vkCreateRenderPass accepts exactly one subpass, hence no next subpass is
     * reachable. Do not mutate render-pass state or append a fake operation. */
    (void)contents;
    ps5vk_command_invalidate(c);
}
VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer c,
    uint32_t count, const VkCommandBuffer *commands)
{
    /* vkAllocateCommandBuffers rejects secondary level and Vulkan requires a
     * nonzero array of secondary buffers here. There is no valid no-op subset. */
    (void)count; (void)commands;
    ps5vk_command_invalidate(c);
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
    const VkViewport *viewport = p->dynamic_viewport ?
        (c->viewport_valid ? &c->viewport : NULL) : &p->viewport;
    const VkRect2D *scissor = p->dynamic_scissor ?
        (c->scissor_valid ? &c->scissor : NULL) : &p->scissor;
    if (!viewport || !scissor) { invalid(c); return; }
    if(p->push_constant_size && (!c->push_constants_valid ||
        memcmp(p->push_constant_stages,c->push_constant_stages,
               sizeof(c->push_constant_stages)))) {invalid(c);return;}
    if(p->set_count>PS5VK_MAX_SETS){invalid(c);return;}
    for(unsigned s=0;s<p->set_count;++s)if(p->sets[s].count &&
        (!c->graphics_sets[s] ||
         memcmp(&p->sets[s],&c->graphics_set_signatures[s],sizeof(p->sets[s])))) {invalid(c);return;}
    VkRenderPass pass = c->render_pass;
    VkFormat depth = pass->depth.attachment == VK_ATTACHMENT_UNUSED ? VK_FORMAT_UNDEFINED :
        pass->attachments[pass->depth.attachment].format;
    if (p->color_format != pass->attachments[pass->color.attachment].format || p->depth_format != depth) {
        invalid(c); return;
    }
    struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,PS5VK_DRAW,
        PS5VK_OPERATION_INSIDE_RENDER_PASS,1);
    if(!op)return;
    op->pipeline=p;op->render_pass=pass;op->framebuffer=c->framebuffer;
    op->viewport=*viewport;op->scissor=*scissor;op->vertex_count=vertices;
    op->instance_count=instances;op->first_vertex=first_vertex;op->first_instance=first_instance;
    memcpy(op->vertices,c->vertices,sizeof(c->vertices));
    op->push_constant_size=p->push_constant_size;
    if(p->push_constant_size)memcpy(op->push_constants,c->push_constants,p->push_constant_size);
    for(unsigned s=0;s<p->set_count;++s)if(p->sets[s].count) {
        op->sets[s]=c->graphics_sets[s];
        op->generations[s]=c->graphics_sets[s]->generation;
    }
    for(uint32_t j=0;j<p->program.descriptor_count;++j) {
        const struct ps5vk_program_descriptor *binding=&p->program.descriptors[j];
        uint32_t index=c->graphics_sets[binding->set]->signature.binding[binding->binding].first+
            binding->element;
        op->descriptor_dynamic_offsets[j]=c->graphics_set_dynamic_offsets[binding->set][index];
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
static int indirect_draw_valid(VkCommandBuffer c,VkBuffer buffer,VkDeviceSize offset,
    uint32_t count,uint32_t stride,VkDeviceSize command_size)
{
    if(!c || (offset&3u) || count>1 ||
       (count>1 && ((stride&3u) || stride<command_size)))return 0;
    if(count)return indirect_buffer_valid(c,buffer,offset,command_size);
    if(c->state!=PS5VK_RECORDING ||
       !ps5vk_buffer_usage(c->pool->device,buffer,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT))return 0;
    void *address=NULL;VkDeviceSize available=0;
    /* drawCount == 0 does not access command data, so Vulkan imposes no
     * offset-plus-command-size bound in that case.  Still prove that the
     * non-sparse buffer is live and completely bound. */
    return ps5vk_buffer_span(c->pool->device,buffer,0,VK_WHOLE_SIZE,
        &address,&available)==VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer c,VkBuffer buffer,
    VkDeviceSize offset,uint32_t count,uint32_t stride)
{
    if(!indirect_draw_valid(c,buffer,offset,count,stride,sizeof(VkDrawIndirectCommand)))
        {invalid(c);return;}
    vkCmdDraw(c,0,0,0,0);if(!c || c->state!=PS5VK_RECORDING)return;
    struct ps5vk_operation *op=&c->operations[c->operation_count-1];
    op->type=PS5VK_DRAW_INDIRECT;op->indirect_buffer=buffer;
    op->indirect_offset=offset;op->indirect_count=count;op->indirect_stride=stride;
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer c,VkBuffer buffer,
    VkDeviceSize offset,uint32_t count,uint32_t stride)
{
    if(!indirect_draw_valid(c,buffer,offset,count,stride,
        sizeof(VkDrawIndexedIndirectCommand))){invalid(c);return;}
    vkCmdDrawIndexed(c,0,0,0,0,0);if(!c || c->state!=PS5VK_RECORDING)return;
    struct ps5vk_operation *op=&c->operations[c->operation_count-1];
    op->type=PS5VK_DRAW_INDEXED_INDIRECT;op->indirect_buffer=buffer;
    op->indirect_offset=offset;op->indirect_count=count;op->indirect_stride=stride;
}
static int texture_layout_supported(VkImageLayout layout)
{
    return layout==VK_IMAGE_LAYOUT_GENERAL || layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
        /* Reachable only through the depth clear-target profile below; every
         * other usage profile refuses it. */
        layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
        layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}
static int texture_scope(VkPipelineStageFlags stages, VkAccessFlags access)
{
    const VkPipelineStageFlags allowed=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT |
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkAccessFlags supported=VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT |
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    if(!stages || (stages & ~allowed) || (access & ~supported))return 0;
    if(!access)return 1;
    if((access & (VK_ACCESS_HOST_READ_BIT|VK_ACCESS_HOST_WRITE_BIT)) &&
        !(stages & VK_PIPELINE_STAGE_HOST_BIT))return 0;
    if((access & VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    if((access & (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)) &&
        !(stages & (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    if((access & (VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT)) &&
        !(stages & (VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    if((access & VK_ACCESS_SHADER_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    if((access & VK_ACCESS_INDIRECT_COMMAND_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    /* Depth/stencil attachment access happens in the fragment tests, which is
     * where a depth-tested draw reads the value an explicit clear wrote. */
    if((access & (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)) &&
        !(stages & (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    /* MEMORY_READ/WRITE select every read/write access available in the stage
     * mask and are valid with any non-empty supported stage mask. */
    return 1;
}
static int compute_scope(VkPipelineStageFlags stages, VkAccessFlags access)
{
    const VkPipelineStageFlags allowed=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkAccessFlags supported=VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT |
        VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    if(!stages || (stages & ~allowed) || (access & ~supported))return 0;
    if(!access)return 1;
    /* ALL_COMMANDS expands queue commands, not host operations. */
    if((access & (VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT)) &&
        !(stages & VK_PIPELINE_STAGE_HOST_BIT))return 0;
    if((access & (VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                  VK_ACCESS_SHADER_WRITE_BIT)) &&
        !(stages & (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    /* Generic MEMORY_READ/WRITE do not require a more specific stage. */
    return 1;
}
static int command_scope(VkPipelineStageFlags stages,VkAccessFlags access)
{ return texture_scope(stages,access) || compute_scope(stages,access); }
static int image_barrier_profile(const VkImageMemoryBarrier *b)
{
    VkImage image=b->image;
    const VkImageUsageFlags usage=image->info.usage;
    const int upload=(usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT))==
        (VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        !(usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    const int readback=(usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT))==
        (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        !(usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    if(upload)return
        (b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
         b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         !b->srcAccessMask && b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT) ||
        (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         b->newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
         b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT &&
         b->dstAccessMask==VK_ACCESS_SHADER_READ_BIT);
    /* Depth clear target: the transition that lets an explicit
     * vkCmdClearDepthStencilImage control a later depth-tested draw. Bounded to
     * exactly the write the clear performed and the access the fragment tests
     * perform; the image never becomes a transfer source or a sampled image. */
    if(ps5vk_depth_clear_image(image))return
        (b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
         b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         !b->srcAccessMask && b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT) ||
        (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         b->newLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
         b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT &&
         b->dstAccessMask==(VkAccessFlags)(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
                                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
    if(readback)return
        ps5vk_color_discard_barrier(b) ||
        (b->oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
         b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
         b->srcAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
         b->dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT);
    /* Pure transfer role: host-visible memory that no GPU stage samples or
     * renders into, so every transition among the transfer layouts is honest
     * bookkeeping. Only transfer dependencies can order such an image. */
    if(ps5vk_pure_transfer_image(image))
        return !((b->srcAccessMask|b->dstAccessMask) &
                 ~(VkAccessFlags)(VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT)) &&
            (b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED ||
             b->oldLayout==VK_IMAGE_LAYOUT_GENERAL ||
             b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
             b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) &&
            (b->newLayout==VK_IMAGE_LAYOUT_GENERAL ||
             b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
             b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    return 0;
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count,
    const VkBufferMemoryBarrier *buffers, uint32_t image_count, const VkImageMemoryBarrier *images)
{
    if (!c || c->state != PS5VK_RECORDING || c->render_pass || !src || !dst || flags ||
        (memory_count && !memory) || (buffer_count && !buffers) || (image_count && !images))
        { invalid(c); return; }
    /* Preserve the existing compute encoding: calls without image barriers
     * carry one aggregate dependency in addition to per-buffer lifetime
     * records. Mixed CTS calls need the aggregate only for memory barriers. */
    const unsigned aggregate=(memory_count || !image_count)?1u:0u;
    if(buffer_count>PS5VK_MAX_OPERATIONS || image_count>PS5VK_MAX_OPERATIONS-buffer_count ||
       aggregate>PS5VK_MAX_OPERATIONS-buffer_count-image_count ||
       c->operation_count>PS5VK_MAX_OPERATIONS-buffer_count-image_count-aggregate)
        { invalid(c); return; }
    VkAccessFlags src_access = 0, dst_access = 0;
    for (uint32_t j = 0; j < memory_count; ++j) {
        if (memory[j].sType != VK_STRUCTURE_TYPE_MEMORY_BARRIER || memory[j].pNext ||
            !command_scope(src,memory[j].srcAccessMask) ||
            !command_scope(dst,memory[j].dstAccessMask)) { invalid(c); return; }
        src_access |= memory[j].srcAccessMask; dst_access |= memory[j].dstAccessMask;
    }
    /* A full cache dependency is stronger than a buffer-range dependency.
     * Retain the buffer/range for lifetime validation; never ignore ownership
     * transfers or accept a range outside its bound allocation. */
    for (uint32_t j = 0; j < buffer_count; ++j) {
        const VkBufferMemoryBarrier *b = &buffers[j];
        void *address; VkDeviceSize bytes;
        if (b->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER || b->pNext ||
            !command_scope(src,b->srcAccessMask) || !command_scope(dst,b->dstAccessMask) ||
            b->srcQueueFamilyIndex != b->dstQueueFamilyIndex ||
            (b->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED && b->srcQueueFamilyIndex != 0) ||
            ps5vk_buffer_span(c->pool->device, b->buffer, b->offset, b->size,
                &address, &bytes) != VK_SUCCESS) { invalid(c); return; }
    }
    for(uint32_t j=0;j<image_count;++j) {
        const VkImageMemoryBarrier *b=&images[j];
        VkImage image=b->image;void *address;VkDeviceSize bytes;
        if(!c->pool->device->graphics_enabled ||
            b->sType!=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER || b->pNext ||
            !command_scope(src,b->srcAccessMask) || !command_scope(dst,b->dstAccessMask) ||
            (!texture_layout_supported(b->oldLayout) && b->oldLayout!=VK_IMAGE_LAYOUT_UNDEFINED) ||
            !texture_layout_supported(b->newLayout) ||
            b->srcQueueFamilyIndex!=b->dstQueueFamilyIndex ||
            (b->srcQueueFamilyIndex!=0 && b->srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED) ||
            !image || image->device!=c->pool->device ||
            !image_barrier_profile(b) ||
            /* A depth target is ordered through its depth aspect; every other
             * role in this profile is colour. */
            b->subresourceRange.aspectMask!=(ps5vk_depth_clear_image(image)?
                (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT:
                (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT) ||
            b->subresourceRange.baseMipLevel || b->subresourceRange.baseArrayLayer ||
            (b->subresourceRange.levelCount!=image->info.mipLevels &&
             b->subresourceRange.levelCount!=VK_REMAINING_MIP_LEVELS) ||
            (b->subresourceRange.layerCount!=VK_REMAINING_ARRAY_LAYERS &&
             b->subresourceRange.layerCount!=image->info.arrayLayers) ||
            ps5vk_image_span(c->pool->device,image,&address,&bytes)!=VK_SUCCESS) {invalid(c);return;}
    }
    /* Append only after every member validates: a rejected mixed dependency
     * never leaves a prefix of recorded operations behind. */
    for (uint32_t j = 0; j < buffer_count; ++j)
        c->operations[c->operation_count++] = (struct ps5vk_operation){.type = PS5VK_BARRIER,
            .src_stage = src, .dst_stage = dst, .src_access = buffers[j].srcAccessMask,
            .dst_access = buffers[j].dstAccessMask, .buffer_barrier = buffers[j]};
    if(aggregate)c->operations[c->operation_count++] = (struct ps5vk_operation){.type = PS5VK_BARRIER,
        .src_stage = src, .dst_stage = dst, .src_access = src_access, .dst_access = dst_access};
    for(uint32_t j=0;j<image_count;++j)
        c->operations[c->operation_count++]=(struct ps5vk_operation){.type=PS5VK_IMAGE_BARRIER,
            .image_barrier=images[j],.src_stage=src,.dst_stage=dst,
            .src_access=images[j].srcAccessMask,.dst_access=images[j].dstAccessMask};
}

static void record_event(VkCommandBuffer c, VkEvent event,
    VkPipelineStageFlags stage, enum ps5vk_operation_type type)
{
    if (!c || c->state != PS5VK_RECORDING || c->render_pass ||
        !event || event->device != c->pool->device ||
        !command_scope(stage, 0) || c->operation_count == PS5VK_MAX_OPERATIONS) {
        invalid(c); return;
    }
    struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,type,
        PS5VK_OPERATION_OUTSIDE_RENDER_PASS,1);
    if(!op)return;
    op->event=event;op->src_stage=stage;op->dst_stage=stage;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer c, VkEvent event,
    VkPipelineStageFlags stage)
{ record_event(c, event, stage, PS5VK_EVENT_SET); }

VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer c, VkEvent event,
    VkPipelineStageFlags stage)
{ record_event(c, event, stage, PS5VK_EVENT_RESET); }

VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer c, uint32_t event_count,
    const VkEvent *events, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    uint32_t memory_count, const VkMemoryBarrier *memory,
    uint32_t buffer_count, const VkBufferMemoryBarrier *buffers,
    uint32_t image_count, const VkImageMemoryBarrier *images)
{
    if (!c || c->state != PS5VK_RECORDING || c->render_pass || !event_count || !events ||
        event_count > PS5VK_MAX_OPERATIONS - c->operation_count) { invalid(c); return; }
    for (uint32_t j = 0; j < event_count; ++j)
        if (!events[j] || events[j]->device != c->pool->device) { invalid(c); return; }
    const unsigned aggregate = (memory_count || !image_count) ? 1u : 0u;
    uint64_t barrier_count = (uint64_t)buffer_count + image_count + aggregate;
    if (barrier_count > PS5VK_MAX_OPERATIONS ||
        c->operation_count > PS5VK_MAX_OPERATIONS - barrier_count ||
        event_count > PS5VK_MAX_OPERATIONS - barrier_count - c->operation_count) {
        invalid(c); return;
    }
    unsigned before = c->operation_count;
    vkCmdPipelineBarrier(c, src, dst, 0, memory_count, memory,
        buffer_count, buffers, image_count, images);
    if (c->state != PS5VK_RECORDING) return;
    unsigned barriers = c->operation_count - before;
    memmove(&c->operations[before + event_count], &c->operations[before],
        barriers * sizeof(c->operations[0]));
    for (uint32_t j = 0; j < event_count; ++j)
        c->operations[before + j] = (struct ps5vk_operation){
            .type = PS5VK_EVENT_WAIT, .event = events[j],
            .src_stage = src, .dst_stage = dst,
        };
    c->operation_count += event_count;
}
