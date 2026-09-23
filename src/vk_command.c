#include "vk_command.h"
#if defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5
#include "ps5log.h"
#endif
/* Recorder step markers. The host build has no console transport, so they
 * compile to nothing there; on the console they are what turns "a case died
 * while recording" into the call it died in. */
#if defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5
#define CMD_MARK(...) ps5log_printf(PS5LOG_MARK, __VA_ARGS__)
#else
#define CMD_MARK(...) ((void)0)
#endif
#include "vk_indirect.h"
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
/* A recorded vkCmdExecuteCommands holds raw references to its children. If a
 * child is reset, re-recorded or freed before its parent is submitted, those
 * references would dangle and submit-time validation would read freed memory.
 * Vulkan already says such a parent becomes invalid, so poison every parent
 * that names this child rather than leaving a stale handle behind. The walk is
 * bounded: a device has a list of pools, a pool a list of buffers, and a
 * buffer at most PS5VK_MAX_OPERATIONS operations. */
static void invalidate_parents_referencing(VkCommandBuffer child)
{
    if (!child || !child->pool || !child->pool->device) return;
    VkDevice d = child->pool->device;
    for (VkCommandPool p = d->command_pools; p; p = p->next)
        for (VkCommandBuffer parent = p->buffers; parent; parent = parent->next) {
            if (parent == child || parent->state == PS5VK_INVALID) continue;
            for (unsigned j = 0; j < parent->operation_count; ++j) {
                const struct ps5vk_operation *op = &parent->operations[j];
                if (op->type != PS5VK_EXECUTE_COMMANDS || !op->owned_payload) continue;
                VkCommandBuffer const *named = (VkCommandBuffer const *)op->owned_payload;
                for (uint32_t n = 0; n < op->child_count; ++n)
                    if (named[n] == child) { parent->state = PS5VK_INVALID; break; }
                if (parent->state == PS5VK_INVALID) break;
            }
        }
}

static void clear(VkCommandBuffer c)
{
    for (unsigned j = 0; j < c->operation_count; ++j) {
        struct ps5vk_operation *op = &c->operations[j];
        if (op->owned_payload)
            ps5vk_object_free(op->owned_payload, &op->payload_allocator,
                op->custom_payload_allocator);
    }
    if (c->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
        invalidate_parents_referencing(c);
    c->state = PS5VK_INITIAL; c->usage = 0; c->pipeline = NULL;
    c->operation_count = 0;
    /* The level survives every reset: Vulkan has no operation that changes it.
     * The inheritance copy does not, because it belongs to one recording. */
    c->inheritance_valid = VK_FALSE;
    memset(&c->inheritance, 0, sizeof(c->inheritance));
    c->graphics_pipeline = NULL; c->render_pass = NULL; c->framebuffer = NULL;
    c->render_pass_inherited = VK_FALSE;
    c->render_pass_contents = VK_SUBPASS_CONTENTS_INLINE;
    c->subpass = 0;
    c->viewport_valid = c->scissor_valid = 0;
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
#define invalid(c) do { \
    CMD_MARK("PS5VK_CMD_REFUSE site=%s:%d", __func__, __LINE__); \
    ps5vk_command_invalidate(c); \
} while (0)

struct ps5vk_operation *ps5vk_command_reserve_operations(VkCommandBuffer c,
    enum ps5vk_operation_type type, enum ps5vk_operation_scope scope, uint32_t count)
{
    if (!c || c->state != PS5VK_RECORDING || !count ||
        scope < PS5VK_OPERATION_OUTSIDE_RENDER_PASS ||
        scope > PS5VK_OPERATION_ANYWHERE ||
        (scope == PS5VK_OPERATION_OUTSIDE_RENDER_PASS && c->render_pass) ||
        (scope == PS5VK_OPERATION_INSIDE_RENDER_PASS && !c->render_pass) ||
        /* A subpass begun with VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
         * carries no work of its own: the only commands legal inside it are
         * vkCmdExecuteCommands, which records with ANYWHERE scope, and the two
         * that close the subpass or the pass. An inline draw there is refused
         * rather than silently recorded into a scope that will never execute
         * it. */
        (scope == PS5VK_OPERATION_INSIDE_RENDER_PASS && c->render_pass &&
         !c->render_pass_inherited && type != PS5VK_END_RENDER_PASS &&
         type != PS5VK_NEXT_SUBPASS &&
         c->render_pass_contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS) ||
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
        (info->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY &&
         info->level != VK_COMMAND_BUFFER_LEVEL_SECONDARY)) return INVALID;
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
        c->pool = p; c->level = info->level; c->next = list; list = c; out[j] = c;
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
            /* Poison any parent that names this child BEFORE the object goes
             * away, so no recorded reference can outlive its target. */
            if (buffers[j]->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
                invalidate_parents_referencing(buffers[j]);
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
/* Two attachment REFERENCES are compatible when both are VK_ATTACHMENT_UNUSED,
 * or both are used and the attachments they refer to agree on format and
 * sample count. The numeric index is not part of it: two passes may reach the
 * same attachment through different slots. */
static VkBool32 reference_compatible(VkRenderPass a, const VkAttachmentReference *ra,
    VkRenderPass b, const VkAttachmentReference *rb)
{
    const VkBool32 a_used = ra->attachment != VK_ATTACHMENT_UNUSED ? VK_TRUE : VK_FALSE;
    const VkBool32 b_used = rb->attachment != VK_ATTACHMENT_UNUSED ? VK_TRUE : VK_FALSE;
    if (a_used != b_used) return VK_FALSE;
    if (!a_used) return VK_TRUE;
    if (ra->attachment >= a->attachment_count ||
        rb->attachment >= b->attachment_count) return VK_FALSE;
    const VkAttachmentDescription *da = &a->attachments[ra->attachment];
    const VkAttachmentDescription *db = &b->attachments[rb->attachment];
    return da->format == db->format && da->samples == db->samples ? VK_TRUE : VK_FALSE;
}

/* Render-pass compatibility, Vulkan 1.0 chapter 7.2. It is defined on the
 * corresponding attachment REFERENCES, and everything else is deliberately
 * excluded: initial and final layouts, load and store ops, and the layout
 * inside a reference are all allowed to differ, so a secondary recorded
 * against a LOAD pass may run inside a CLEAR pass of the same shape.
 *
 * Three things this must NOT compare, each of which would refuse a conformant
 * call: the total attachment count, because attachments no reference names are
 * irrelevant and the arrays may differ in length; the numeric attachment
 * indices, because the same role may sit in different slots; and object
 * identity, because compatibility is a property of shape, not of handles. */
VkBool32 ps5vk_render_pass_compatible(VkRenderPass a, VkRenderPass b)
{
    if (!a || !b) return VK_FALSE;
    if (a == b) return VK_TRUE;
    /* Compatibility is per subpass and positional: subpass i of one pass is
     * compared with subpass i of the other, and two passes with different
     * subpass counts describe different scopes entirely. */
    if (a->subpass_count != b->subpass_count) return VK_FALSE;
    for (uint32_t i = 0; i < a->subpass_count; ++i) {
        const struct ps5vk_subpass *left = ps5vk_render_pass_subpass(a, i);
        const struct ps5vk_subpass *right = ps5vk_render_pass_subpass(b, i);
        /* Two render passes are compatible only when their subpasses agree on
         * every colour reference they carry, on the resolve target each colour
         * reference names, and on the depth one. */
        if (left->color_count != right->color_count) return VK_FALSE;
        for (uint32_t c = 0; c < left->color_count; ++c)
            if (!reference_compatible(a, &left->color[c], b, &right->color[c]))
                return VK_FALSE;
        /* A resolve array is part of the subpass's shape: one pass declaring
         * one and the other not is a different subpass, not a compatible one. */
        if (left->resolve_count != right->resolve_count) return VK_FALSE;
        for (uint32_t c = 0; c < left->resolve_count; ++c)
            if (!reference_compatible(a, &left->resolve[c], b, &right->resolve[c]))
                return VK_FALSE;
        if (!reference_compatible(a, &left->depth, b, &right->depth)) return VK_FALSE;
    }
    return VK_TRUE;
}

/* Inheritance a secondary may declare, checked against what this device can
 * truthfully do rather than against the structure's shape.
 *
 * WITH VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT the scope members stop
 * being decorative: renderPass and subpass name the scope the secondary will
 * be executed in, so they are validated here. The named subpass must exist in
 * the inherited render pass; an out-of-range index is refused rather than
 * clamped or treated as subpass zero. framebuffer is OPTIONAL by the
 * specification and VK_NULL_HANDLE is accepted: the driver
 * takes the framebuffer from the executing primary, and refusing the null
 * handle would reject a conformant call.
 *
 * Without that flag Vulkan IGNORES renderPass, framebuffer and subpass, which
 * the pinned CTS states explicitly (doc/testspecs/VK/apitests.adoc,
 * command-buffer-recording case 9: "Otherwise the renderPass, framebuffer, and
 * subpass members of the VkCommandBufferBeginInfo structure are ignored").
 * So whatever the caller puts there is accepted and never consulted. Refusing
 * a non-null handle would reject a conformant call; this driver simply does
 * not read those members. */
static VkResult inheritance_valid(VkDevice d, const VkCommandBufferInheritanceInfo *i,
    VkCommandBufferUsageFlags usage)
{
    if (!i || i->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO ||
        i->pNext) return INVALID;
    if (usage & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) {
        if (!d->graphics_enabled || !i->renderPass ||
            i->renderPass->device != d ||
            /* The inherited subpass must exist in the inherited pass. */
            i->subpass >= i->renderPass->subpass_count ||
            (i->framebuffer &&
             (i->framebuffer->device != d ||
              !ps5vk_framebuffer_compatible(i->framebuffer, i->renderPass))))
            return INVALID;
    }
    /* occlusionQueryEnable, queryFlags and pipelineStatistics describe queries
     * this device does not execute: it reports occlusionQueryPrecise and
     * pipelineStatisticsQuery false and no query command is implemented, so a
     * secondary that claims to inherit one is refused instead of recorded. */
    if (i->occlusionQueryEnable || i->queryFlags || i->pipelineStatistics)
        return INVALID;
    (void)d;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *info)
{
    if (!c || !info || info->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO ||
        c->state == PS5VK_PENDING || c->state == PS5VK_RECORDING ||
        /* VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT is meaningful only
         * for a secondary. For a primary VUID-vkBeginCommandBuffer-flags-09123
         * makes it IGNORED - and the pinned upstream render-pass module does
         * set it on its primary buffers - so it is accepted there and never
         * consulted: the inheritance info is not read for a primary either. */
        (info->flags & ~(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                         VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT |
                         VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) ||
        /* VUID-vkBeginCommandBuffer-commandBuffer-02840 makes the two usage
         * flags mutually exclusive for a PRIMARY only. A secondary may set
         * both, so refusing the pair there would reject a conformant call. */
        (c->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY &&
         (info->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) &&
         (info->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT))) return INVALID;
    if (info->pNext) {
        const VkDeviceGroupCommandBufferBeginInfo *group =
            (const VkDeviceGroupCommandBufferBeginInfo *)info->pNext;
        if (!c->pool->device->device_group_extension_enabled ||
            group->sType != VK_STRUCTURE_TYPE_DEVICE_GROUP_COMMAND_BUFFER_BEGIN_INFO ||
            group->pNext || group->deviceMask != 1) return INVALID;
    }
    if (c->state != PS5VK_INITIAL && !(c->pool->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT)) return INVALID;
    /* A secondary must describe what it inherits; a primary must not, and
     * Vulkan says the pointer is ignored for one, so it is not stored. */
    if (c->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY &&
        inheritance_valid(c->pool->device, info->pInheritanceInfo, info->flags) != VK_SUCCESS)
        return INVALID;
    clear(c); c->usage = info->flags; c->state = PS5VK_RECORDING;
    if (c->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
        /* Retained as an opaque owned copy of what the caller supplied. When
         * RENDER_PASS_CONTINUE is absent, the ignored scope members are
         * preserved verbatim and never consulted. With the flag present they
         * were validated above and seed the inherited recording scope. */
        c->inheritance = *info->pInheritanceInfo;
        c->inheritance_valid = VK_TRUE;
        /* A continuation secondary records INSIDE the inherited pass from its
         * first command. Entering the scope here is what lets every existing
         * draw path work unchanged in a secondary: they already record against
         * c->render_pass and refuse to record outside one. */
        if (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) {
            c->render_pass = c->inheritance.renderPass;
            c->framebuffer = c->inheritance.framebuffer;
            c->subpass = c->inheritance.subpass;
            c->render_pass_inherited = VK_TRUE;
        }
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c)
{
    /* A pass this buffer BEGAN must be ended before recording stops; an
     * INHERITED one must not, because the primary owns it and the secondary
     * has no vkCmdEndRenderPass to give. */
    if (!c || c->state != PS5VK_RECORDING ||
        (c->render_pass && !c->render_pass_inherited)) return INVALID;
    c->state = PS5VK_EXECUTABLE;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint point, VkPipeline p)
{
    CMD_MARK("PS5VK_CMD_BIND_PIPELINE subpass=%u samples=%u",
        p ? (unsigned)p->subpass : 0xffffffffu,
        p ? (unsigned)p->samples : 0u);
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
/* Core viewport/scissor array rules shared by both setters: first + count is
 * between 1 and the array capacity, and without multiViewport ENABLED on the
 * logical device first is 0 and count is 1. Every element is validated before
 * any is stored, so a rejected call leaves the previous state whole. */
static int viewport_range(VkCommandBuffer c, uint32_t first, uint32_t count)
{
    if (!count || first > PS5VK_MAX_VIEWPORTS - count) return 0;
    if ((first || count != 1) &&
        !(c->pool->device->enabled_features & PS5VK_FEATURE_MULTI_VIEWPORT)) return 0;
    return 1;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer c, uint32_t first,
    uint32_t count, const VkViewport *viewports)
{
    if (!c || c->state != PS5VK_RECORDING || !viewport_range(c, first, count) ||
        !viewports) { invalid(c); return; }
    for (uint32_t i = 0; i < count; ++i)
        if (!valid_viewport(&viewports[i])) { invalid(c); return; }
    for (uint32_t i = 0; i < count; ++i) c->viewports[first + i] = viewports[i];
    c->viewport_valid |= ((1u << count) - 1u) << first;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer c, uint32_t first,
    uint32_t count, const VkRect2D *scissors)
{
    if (!c || c->state != PS5VK_RECORDING || !viewport_range(c, first, count) ||
        !scissors) { invalid(c); return; }
    for (uint32_t i = 0; i < count; ++i)
        if (!valid_scissor(&scissors[i])) { invalid(c); return; }
    for (uint32_t i = 0; i < count; ++i) c->scissors[first + i] = scissors[i];
    c->scissor_valid |= ((1u << count) - 1u) << first;
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
    /* A non-zero clamp is legal only when depthBiasClamp is ENABLED on this
     * logical device (the physical mask is not consulted: an application that
     * did not ask for the feature keeps the rules of a device without it).
     * Vulkan places no finiteness restriction on any of the three factors;
     * retain their float bit patterns without inventing a narrower contract. */
    if (clamp != 0.0f &&
        !(c->pool->device->enabled_features & PS5VK_FEATURE_DEPTH_BIAS_CLAMP)) {
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
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
        VK_SHADER_STAGE_GEOMETRY_BIT;
    if (!c || c->state != PS5VK_RECORDING || !layout ||
        layout->device != c->pool->device || !stages || (stages & ~supported) ||
        !size || !values || (offset & 3u) || (size & 3u) ||
        offset >= PS5VK_MAX_PUSH_CONSTANT_BYTES ||
        size > PS5VK_MAX_PUSH_CONSTANT_BYTES - offset) { invalid(c); return; }
    uint32_t first=offset/4u,end=(offset+size)/4u;
    for(uint32_t j=first;j<end;++j)
        /* 01795: every requested stage covers the byte. 01796: the update
         * also includes every stage of each range overlapping that byte. */
        if(layout->push_constant_stages[j]!=stages){invalid(c);return;}
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
VKAPI_ATTR void VKAPI_CALL vkCmdDispatchBaseKHR(VkCommandBuffer c,
    uint32_t base_x, uint32_t base_y, uint32_t base_z,
    uint32_t count_x, uint32_t count_y, uint32_t count_z)
{
    const uint32_t base[3] = {base_x, base_y, base_z};
    const uint32_t count[3] = {count_x, count_y, count_z};
    if (!c || c->state != PS5VK_RECORDING || !c->pool ||
        !c->pool->device->device_group_extension_enabled || !c->pipeline ||
        ((base_x || base_y || base_z) && !c->pipeline->dispatch_base_enabled)) {
        invalid(c); return;
    }
    for (unsigned n = 0; n < 3; ++n)
        if (base[n] >= 65535 || count[n] > 65535 - base[n]) {
            invalid(c); return;
        }
    const uint32_t before = c->operation_count;
    vkCmdDispatch(c, count_x, count_y, count_z);
    if (c->state == PS5VK_RECORDING && c->operation_count == before + 1)
        memcpy(c->operations[before].group_base, base, sizeof(base));
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDeviceMaskKHR(VkCommandBuffer c, uint32_t mask)
{
    /* This logical device has exactly one physical device. Selecting it is a
     * state update with no change to the native command stream. */
    if (!c || c->state != PS5VK_RECORDING || !c->pool ||
        !c->pool->device->device_group_extension_enabled || mask != 1)
        invalid(c);
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
    /* Announced before the call decides anything: a case that dies while
     * recording leaves the begin as the last thing the log names, which is how
     * a crash in the recorder is told from one in the objects it is handed. */
    CMD_MARK("PS5VK_CMD_BEGIN_RENDER_PASS clear_values=%u",
        info ? info->clearValueCount : 0u);
    /* Primary-only: a secondary inherits a render pass, it never begins one. */
    if (!c || c->state != PS5VK_RECORDING || c->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
        !c->pool->device->graphics_enabled || c->render_pass ||
        !info || info->sType != VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO ||
        (contents != VK_SUBPASS_CONTENTS_INLINE &&
         contents != VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS) ||
        !info->renderPass || !info->framebuffer ||
        info->renderPass->device != c->pool->device || info->framebuffer->device != c->pool->device ||
        /* One value per attachment the pass may name, which is the bound the
         * begin's own pass carries: the pinned multisample oracle clears four
         * attachments in one begin. */
        info->clearValueCount > PS5VK_MAX_ATTACHMENTS ||
        (info->clearValueCount && !info->pClearValues) ||
        c->operation_count == PS5VK_MAX_OPERATIONS) { invalid(c); return; }
    VkRenderPass pass = info->renderPass; VkFramebuffer fb = info->framebuffer;
    VkRect2D area = info->renderArea;
    if (info->pNext) {
        const VkDeviceGroupRenderPassBeginInfo *group =
            (const VkDeviceGroupRenderPassBeginInfo *)info->pNext;
        if (!c->pool->device->device_group_extension_enabled ||
            group->sType != VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO ||
            group->pNext || group->deviceMask != 1 ||
            group->deviceRenderAreaCount > 1 ||
            (group->deviceRenderAreaCount && !group->pDeviceRenderAreas)) {
            invalid(c); return;
        }
        if (group->deviceRenderAreaCount) area = group->pDeviceRenderAreas[0];
    }
    /* Every colour role the subpass names must be the one the framebuffer
     * carries, in order, and the depth role after them. A subpass that names
     * no colour role at all - the DEPTH-ONLY shape - has an empty list on both
     * sides, so the loop below simply does not run. A resolve role is part of
     * the executing framebuffer contract too: when the subpass declares one,
     * the framebuffer has to carry it at the same index (DXVK262-T06). */
    const struct ps5vk_subpass *first=ps5vk_render_pass_subpass(pass, 0);
    if (fb->attachment_count != pass->attachment_count ||
        fb->color_count != first->color_count ||
        fb->resolve_count != first->resolve_count ||
        (fb->resolve_count && fb->resolve_attachments[0] != first->resolve[0].attachment) ||
        fb->depth_attachment != first->depth.attachment ||
        area.offset.x < 0 || area.offset.y < 0 ||
        !area.extent.width || !area.extent.height || (uint32_t)area.offset.x > fb->width ||
        (uint32_t)area.offset.y > fb->height || area.extent.width > fb->width - (uint32_t)area.offset.x ||
        area.extent.height > fb->height - (uint32_t)area.offset.y) { invalid(c); return; }
    for (uint32_t role = 0; role < first->color_count; ++role)
        if (fb->color_attachments[role] != first->color[role].attachment) { invalid(c); return; }
    for (uint32_t j = 0; j < pass->attachment_count; ++j) {
        const VkAttachmentDescription *a = &pass->attachments[j];
        if (fb->formats[j] != a->format || fb->samples[j] != a->samples ||
            (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && info->clearValueCount <= j)) { invalid(c); return; }
    }
    struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,PS5VK_BEGIN_RENDER_PASS,
        PS5VK_OPERATION_OUTSIDE_RENDER_PASS,1);
    if(!op)return;
    op->render_pass=pass;op->framebuffer=fb;op->render_area=area;
    op->render_pass_contents=contents;
    op->subpass=0;
    op->clear_count=info->clearValueCount;
    if (info->clearValueCount) memcpy(op->clears, info->pClearValues, info->clearValueCount * sizeof(VkClearValue));
    c->render_pass = pass; c->framebuffer = fb; c->render_pass_contents = contents;
    c->subpass = 0;
}

/* Advance to the next subpass.
 *
 * The transition is exact: it is primary-only, it needs an open pass that this
 * buffer began, and there must BE a next subpass. Empty subpasses are legal to
 * record. Each subpass carries its own contents mode, so a pass may take its
 * first subpass inline and name secondaries in the second.
 *
 * The transition is RECORDED as an operation rather than kept only in
 * recording state, so submission re-derives the subpass structure from the
 * immutable record instead of trusting what the recorder remembered. */
VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer c,
    VkSubpassContents contents)
{
    if (!c || c->state != PS5VK_RECORDING ||
        c->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY || !c->render_pass ||
        c->render_pass_inherited ||
        (contents != VK_SUBPASS_CONTENTS_INLINE &&
         contents != VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS) ||
        c->subpass + 1 >= c->render_pass->subpass_count) { invalid(c); return; }
    struct ps5vk_operation *op = ps5vk_command_reserve_operations(c, PS5VK_NEXT_SUBPASS,
        PS5VK_OPERATION_INSIDE_RENDER_PASS, 1);
    if (!op) return;
    op->render_pass = c->render_pass; op->framebuffer = c->framebuffer;
    op->render_pass_contents = contents;
    op->subpass = c->subpass + 1;
    c->subpass = op->subpass;
    c->render_pass_contents = contents;
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer c)
{
    /* Primary-only, and unreachable in a secondary anyway because one can
     * never have begun a render pass; the level check keeps the rejection
     * explicit rather than incidental.
     *
     * An EMPTY subpass and an empty render pass are legal Vulkan - load and
     * store ops alone are observable - so recording them is accepted and
     * recorded faithfully. What this driver cannot EXECUTE is refused where
     * execution is decided: submission refuses a pass that carries no work,
     * and the native backend refuses it independently. Refusing it here would
     * reject a conformant program instead of admitting an unimplemented one.
     *
     * A multi-subpass pass must have REACHED its last subpass, which is a
     * structural requirement of the recording rather than a judgement about
     * work: ending early would silently drop the subpasses never entered. */
    if (!c || c->state != PS5VK_RECORDING || c->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
        !c->render_pass || c->subpass + 1 != c->render_pass->subpass_count ||
        c->operation_count == PS5VK_MAX_OPERATIONS) { invalid(c); return; }
    struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,PS5VK_END_RENDER_PASS,
        PS5VK_OPERATION_INSIDE_RENDER_PASS,1);
    if(!op)return;
    op->render_pass=c->render_pass;op->framebuffer=c->framebuffer;
    op->subpass=c->subpass;
    c->render_pass = NULL; c->framebuffer = NULL; c->subpass = 0;
}
/* Record an ordered, owned list of secondary references.
 *
 * Nothing is copied or flattened: the operation NAMES the children and the
 * queue expands each name into its own submission segment, so a child keeps
 * its object identity and its reuse rules. A duplicate reference is legal
 * under simultaneous use and simply names the child twice.
 *
 * Refused, each poisoning the recording with no partial operation left:
 * nesting, an empty or null array, a child of another device or without a
 * pool, a non-secondary child, a child that is neither pending nor executable,
 * a self-reference, and a pending or repeated child that was not recorded for
 * simultaneous use. There is no invented limit on how many children may be
 * named.
 *
 * The render-pass scope must MATCH in both directions. Inside a pass the pass
 * must have been begun with VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS and
 * every child must be a continuation recorded against a compatible render pass
 * and the same subpass; outside a pass a continuation child is refused,
 * because its recorded draws have no scope to execute in. */
VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer c,
    uint32_t count, const VkCommandBuffer *commands)
{
    if (!c || c->state != PS5VK_RECORDING ||
        /* Primary-only: a secondary can never execute another buffer. */
        c->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
        /* An INLINE pass carries its own draws and admits no secondaries. */
        (c->render_pass &&
         c->render_pass_contents != VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS) ||
        !count || !commands) { invalid(c); return; }
    VkDevice d = c->pool->device;
    for (uint32_t j = 0; j < count; ++j) {
        VkCommandBuffer child = commands[j];
        const VkBool32 simultaneous =
            (child && (child->usage & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)) ?
            VK_TRUE : VK_FALSE;
        const VkBool32 continues = (child &&
            (child->usage & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) ?
            VK_TRUE : VK_FALSE;
        /* VUID-vkCmdExecuteCommands-pCommandBuffers-00089: a child must be in
         * the PENDING OR EXECUTABLE state, so pending is legal; 00091 narrows
         * that to simultaneous-use buffers only. Demanding EXECUTABLE
         * unconditionally would refuse a conformant call. */
        if (!child || child == c || !child->pool || child->pool->device != d ||
            child->level != VK_COMMAND_BUFFER_LEVEL_SECONDARY ||
            (child->state != PS5VK_EXECUTABLE &&
             !(child->state == PS5VK_PENDING && simultaneous)) ||
            (c->render_pass ? !continues : continues))
            { invalid(c); return; }
        /* Inside a pass the inherited scope must actually match the one the
         * child is about to execute in: a compatible render pass, the exact
         * current subpass, and either the primary's framebuffer or the
         * null handle the specification allows a secondary to inherit. */
        if (c->render_pass &&
            (!child->inheritance_valid ||
             !ps5vk_render_pass_compatible(child->inheritance.renderPass, c->render_pass) ||
             /* The child must have been recorded for the subpass it is about
              * to execute in, not merely for some subpass of a compatible
              * pass: its draws were validated against that subpass's formats
              * and its pipelines carry that identity. */
             child->inheritance.subpass != c->subpass ||
             (child->inheritance.framebuffer &&
              child->inheritance.framebuffer != c->framebuffer)))
            { invalid(c); return; }
        if (!simultaneous) {
            /* Without simultaneous use a child may not already be pending and
             * may not appear twice: either executes it alongside itself. */
            if (child->pending_count) { invalid(c); return; }
            for (uint32_t k = 0; k < j; ++k)
                if (commands[k] == child) { invalid(c); return; }
        }
    }
    /* Validate the whole call before appending, and own the array, so a
     * rejected call leaves nothing behind and a later caller mutation cannot
     * change which children execute. */
    struct ps5vk_operation *op = ps5vk_command_reserve_operation_with_payload(c,
        /* Legal in both scopes, each already checked exactly above. */
        PS5VK_EXECUTE_COMMANDS, PS5VK_OPERATION_ANYWHERE,
        commands, count * sizeof(*commands));
    if (!op) return;
    op->child_count = count;
    /* The scope this call was recorded in, so submission can re-derive which
     * rules applied without replaying the primary's recording state. */
    op->render_pass = c->render_pass;
    op->framebuffer = c->framebuffer;
    op->subpass = c->subpass;
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
    CMD_MARK("PS5VK_CMD_DRAW vertices=%u instances=%u",
        vertices,instances);
    if (!c || c->state != PS5VK_RECORDING || !c->render_pass || !c->graphics_pipeline ||
        c->operation_count == PS5VK_MAX_OPERATIONS) { invalid(c); return; }
    VkPipeline p = c->graphics_pipeline;
    /* The pipeline's viewport_count decides how many indices the draw needs.
     * Dynamic arrays must have every one of those indices set; indices at or
     * above the count are neither required nor copied. */
    const uint32_t viewport_count = p->viewport_count;
    if (!viewport_count || viewport_count > PS5VK_MAX_VIEWPORTS) { invalid(c); return; }
    const uint32_t needed = ((1u << viewport_count) - 1u);
    const VkViewport *viewport = p->dynamic_viewport ?
        ((c->viewport_valid & needed) == needed ? c->viewports : NULL) : p->viewports;
    const VkRect2D *scissor = p->dynamic_scissor ?
        ((c->scissor_valid & needed) == needed ? c->scissors : NULL) : p->scissors;
    if (!viewport || !scissor) { invalid(c); return; }
    /* Resolve the rasterization snapshot now, by value. Dynamic depth-bias
     * factors are required only when the bias is enabled: Vulkan ignores them
     * otherwise, so an unset dynamic state does not invalidate a draw whose
     * pipeline has the bias disabled. */
    struct ps5vk_raster_state raster=p->raster;
    if(p->dynamic_depth_bias && raster.depth_bias_enable) {
        if(!(c->dynamic_state_valid & PS5VK_DYNAMIC_DEPTH_BIAS)) {invalid(c);return;}
        raster.depth_bias_constant=c->depth_bias_constant;
        raster.depth_bias_clamp=c->depth_bias_clamp;
        raster.depth_bias_slope=c->depth_bias_slope;
    }
    if(p->push_constant_size && (!c->push_constants_valid ||
        memcmp(p->push_constant_stages,c->push_constant_stages,
               sizeof(c->push_constant_stages)))) {invalid(c);return;}
    if(p->set_count>PS5VK_MAX_SETS){invalid(c);return;}
    for(unsigned s=0;s<p->set_count;++s)if(ps5vk_graphics_set_required(p,s) &&
        (!c->graphics_sets[s] ||
         memcmp(&p->sets[s],&c->graphics_set_signatures[s],sizeof(p->sets[s])))) {invalid(c);return;}
    VkRenderPass pass = c->render_pass;
    /* A graphics pipeline belongs to ONE subpass. Drawing with a pipeline
     * created for a different one would execute it with the state of a scope
     * it was never compiled for, so it is refused rather than tolerated
     * because the formats happen to agree. */
    if (p->subpass != c->subpass) { invalid(c); return; }
    /* The formats a draw must match are those of the CURRENT subpass, not of
     * the pass as a whole. */
    const struct ps5vk_subpass *subpass = ps5vk_render_pass_subpass(pass, c->subpass);
    VkFormat depth = subpass->depth.attachment == VK_ATTACHMENT_UNUSED ? VK_FORMAT_UNDEFINED :
        pass->attachments[subpass->depth.attachment].format;
    /* A depth-only subpass has no colour reference at all, and its pipeline
     * records a colour count of zero with an undefined colour format, so the
     * two still have to agree exactly; the loop below then compares every
     * colour format the subpass does name, one per attachment. */
    if (p->color_attachment_count != subpass->color_count ||
        p->depth_format != depth) {
        invalid(c); return;
    }
    for (uint32_t attachment = 0; attachment < subpass->color_count; ++attachment)
        if (p->color_format[attachment] !=
            pass->attachments[subpass->color[attachment].attachment].format) {
            invalid(c); return;
        }
    struct ps5vk_operation *op=ps5vk_command_reserve_operations(c,PS5VK_DRAW,
        PS5VK_OPERATION_INSIDE_RENDER_PASS,1);
    if(!op)return;
    op->pipeline=p;op->render_pass=pass;op->framebuffer=c->framebuffer;
    op->subpass=c->subpass;
    op->viewport_count=viewport_count;
    memcpy(op->viewports,viewport,viewport_count*sizeof(*viewport));
    memcpy(op->scissors,scissor,viewport_count*sizeof(*scissor));
    op->raster=raster;op->vertex_count=vertices;
    op->instance_count=instances;op->first_vertex=first_vertex;op->first_instance=first_instance;
    memcpy(op->vertices,c->vertices,sizeof(c->vertices));
    op->push_constant_size=p->push_constant_size;
    if(p->push_constant_size)memcpy(op->push_constants,c->push_constants,p->push_constant_size);
    for(unsigned s=0;s<p->set_count;++s)if(ps5vk_graphics_set_required(p,s)) {
        op->sets[s]=c->graphics_sets[s];
        op->generations[s]=c->graphics_sets[s]->generation;
        memcpy(op->graphics_dynamic_offsets[s],c->graphics_set_dynamic_offsets[s],
            sizeof(op->graphics_dynamic_offsets[s]));
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
    uint32_t count,uint32_t stride,enum ps5vk_operation_type type)
{
    if(!c || c->state!=PS5VK_RECORDING || (offset&3u))return 0;
    VkDevice d=c->pool->device;
    /* drawCount is bounded by the physical maxDrawIndirectCount, and more than
     * one command additionally needs multiDrawIndirect ENABLED on this device:
     * a physical device that could execute many commands does not license an
     * application that did not ask for the feature. The span the call reads -
     * stride rules included - is decided by the same checked helper the
     * queue-head re-validation uses, so recording cannot accept a shape the
     * head later refuses. */
    if(count>d->physical->platform.properties.limits.maxDrawIndirectCount ||
       (count>1 && !(d->enabled_features&PS5VK_FEATURE_MULTI_DRAW_INDIRECT)))return 0;
    VkDeviceSize length=0;
    if(!ps5vk_indirect_argument_span(type,count,stride,&length))return 0;
    if(count)return indirect_buffer_valid(c,buffer,offset,length);
    if(!ps5vk_buffer_usage(d,buffer,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT))return 0;
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
    if(!indirect_draw_valid(c,buffer,offset,count,stride,PS5VK_DRAW_INDIRECT))
        {invalid(c);return;}
    vkCmdDraw(c,0,0,0,0);if(!c || c->state!=PS5VK_RECORDING)return;
    struct ps5vk_operation *op=&c->operations[c->operation_count-1];
    op->type=PS5VK_DRAW_INDIRECT;op->indirect_buffer=buffer;
    op->indirect_offset=offset;op->indirect_count=count;op->indirect_stride=stride;
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer c,VkBuffer buffer,
    VkDeviceSize offset,uint32_t count,uint32_t stride)
{
    if(!indirect_draw_valid(c,buffer,offset,count,stride,PS5VK_DRAW_INDEXED_INDIRECT))
        {invalid(c);return;}
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
        /* ALL_GRAPHICS is the whole graphics pipeline, so it contains every
         * graphics stage named above, including both fragment-test stages.
         * The pinned upstream depth clamp module hands its cleared depth
         * target to the draw with this mask. */
        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
        /* The profile serves compute as well - the packaged compute selections
         * run on it - and the pinned render-pass module's common stage pair
         * (getAllPipelineStageFlags, vktRenderPassTests.cpp:494) names every
         * stage it can run, starting with the compute shader, when it orders
         * an attachment against the engine that reads it back. Naming the
         * stage does not make this a compute dependency: the per-access rules
         * below still decide which accesses this scope can order, and a
         * dependency the compute scope alone can carry is left to it. */
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    /* INDEX_READ, UNIFORM_READ and INPUT_ATTACHMENT_READ are accesses this
     * profile really serves in a graphics command: the promoted index path
     * reads the index buffer, descriptors feed uniform buffers to the vertex
     * and fragment stages, and subpassLoad reads the input attachments of the
     * promoted subpass chain. They were missing here only because no earlier
     * accepted case named them in a barrier; the pinned upstream render-pass
     * module does, because it initializes an attachment with a destination
     * scope of every memory read (vktRenderPassTests.cpp:457). Their stage
     * rules below keep each one tied to the stage that performs it. */
    const VkAccessFlags supported=VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT |
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
        VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
        VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    if(!stages || (stages & ~allowed) || (access & ~supported))return 0;
    if(!access)return 1;
    if((access & (VK_ACCESS_HOST_READ_BIT|VK_ACCESS_HOST_WRITE_BIT)) &&
        !(stages & VK_PIPELINE_STAGE_HOST_BIT))return 0;
    if((access & VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    /* An index read happens in the vertex input stage, exactly where an
     * attribute read happens. */
    if((access & VK_ACCESS_INDEX_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    /* A uniform buffer read happens in a shader stage; this scope's stage mask
     * names the two the profile compiles, and the compute scope carries the
     * compute one. */
    if((access & VK_ACCESS_UNIFORM_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    /* Vulkan reads an input attachment with subpassLoad, which exists only in
     * the fragment shader, so an input-attachment read cannot be ordered by a
     * stage mask that leaves the fragment shader out. ALL_GRAPHICS stands for
     * the whole graphics pipeline and contains it. */
    if((access & VK_ACCESS_INPUT_ATTACHMENT_READ_BIT) &&
        !(stages & (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    if((access & (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)) &&
        !(stages & (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
                    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    if((access & (VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT)) &&
        !(stages & (VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    /* A shader write is ordered by the same stages a shader read is. The
     * pinned upstream clear helper names SHADER_WRITE as the destination
     * access of its post-clear transition, against the fragment stage
     * (vkImageUtil.cpp clearColorImage, the tcu::Vec4 overload), and the
     * compute scope cannot carry it because the fragment stage is not a
     * compute stage. The bit selects which writes become visible; it does not
     * authorize a shader to write anything. */
    if((access & (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)) &&
        !(stages & (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))return 0;
    if((access & VK_ACCESS_SHADER_WRITE_BIT) &&
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
                    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
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
    if(ps5vk_array_color_image(image))return ps5vk_array_color_barrier(b);
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
    /* A depth attachment that also declares the transfer-source role is read
     * back after rendering, so it takes the same two transitions the colour
     * readback takes, in depth's own layouts. Bounded to the role predicate:
     * a depth image without TRANSFER_SRC keeps only the clear transitions
     * below. */
    if(ps5vk_depth_readback_image(image)) {
        if((b->oldLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
            b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            b->srcAccessMask==VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT &&
            b->dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT) ||
           (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            b->newLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
            b->srcAccessMask==VK_ACCESS_TRANSFER_READ_BIT &&
            b->dstAccessMask==(VkAccessFlags)(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
                                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)) ||
           (b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
            b->newLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
            !b->srcAccessMask &&
            !(b->dstAccessMask & ~(VkAccessFlags)(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
                                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT))))
            return 1;
    }
    if(ps5vk_depth_clear_image(image))return
        (b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
         b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         !b->srcAccessMask && b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT) ||
        /* Handing the cleared surface to the draw. The destination access is
         * whatever depth-stencil access the caller declared: the pinned
         * upstream depth clamp module names the write alone, an earlier
         * measured case named both. Requiring the pair would be inventing a
         * requirement; the write itself is not optional, so a read-only
         * destination, an empty one, or a foreign access is still refused. */
        (b->oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         b->newLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
         b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT &&
         (b->dstAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) &&
         !(b->dstAccessMask & ~(VkAccessFlags)(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)));
    if(readback)return
        ps5vk_color_discard_barrier(b) ||
        ps5vk_color_readback_reuse_barrier(b) ||
        (b->oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
         b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
         b->srcAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
         b->dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT);
    /* Colour attachment that also declares a transfer destination: the pinned
     * upstream draw cases initialise it by transitioning UNDEFINED to GENERAL
     * for a transfer write, clear it in GENERAL and then run a GENERAL render
     * pass. Exactly that one transition is accepted here - GENERAL is not a
     * wildcard, and the role predicate keeps it to this single image shape. */
    if(ps5vk_colour_transfer_image(image))return
        (b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
         b->newLayout==VK_IMAGE_LAYOUT_GENERAL &&
         !b->srcAccessMask && b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT) ||
        /* The same image is a readback target whenever it also declares
         * TRANSFER_SRC. The readback role above is written for an image that
         * is only a colour attachment and a transfer source, so it excludes
         * TRANSFER_DST; an upstream case that clears or uploads through the
         * same image and then reads it back declares all three and fell
         * through to the initialise-only rule, which refused the readback
         * transition and invalidated the command buffer
         * (measured: dEQP-VK.draw.renderpass.scissor.* -> vkEndCommandBuffer
         * VK_ERROR_UNKNOWN in the 2026-09-20 measurement run). The transfer
         * destination does not change what the readback transition means, so
         * the same three barriers are accepted here, and only when the image
         * really declares the transfer-source role. */
        ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
         (ps5vk_color_discard_barrier(b) ||
          ps5vk_color_readback_reuse_barrier(b) ||
          (b->oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
           b->newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
           b->srcAccessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT &&
           b->dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT))) ||
        /* Clear through the transfer destination, then render into the same
         * image. The pinned upstream helper clears a colour target outside the
         * render pass and records exactly these two transitions around its
         * vkCmdClearColorImage: the image is acquired as a transfer
         * destination from UNDEFINED, and handed to the colour attachment
         * stage afterwards with the helper's own destination access, which is
         * SHADER_WRITE for the tcu::Vec4 overload the draw module calls.
         *
         * The pinned render-pass module records the same two transitions for
         * the same image shape, but names a whole read scope in the
         * destination instead of one write
         * (pushImageInitializationCommands, vktRenderPassTests.cpp:3150):
         * every memory read the module can later perform on that attachment,
         * plus the write the clear performs, for the acquire; and the same
         * read scope for the handover, naming the access the helper hands the
         * cleared image to: the colour attachment's own access for the
         * render-pass module, SHADER_WRITE for the draw module's clear helper
         * (vkImageUtil.cpp clearColorImage), which is also what the two
         * transitions above accepted before. Both are accepted with the write
         * the transition really needs and the destination bounded to that read
         * scope plus those access flags, so an empty destination, a read-only
         * one, or any foreign access bit still refuses the barrier. */
        (ps5vk_attachment_initialization_acquire_barrier(b) ||
         ps5vk_attachment_initialization_handover_barrier(b)) ||
        /* Handing a rendered attachment to its readback, bounded to exactly
         * the barrier the pinned module records (see
         * ps5vk_colour_readback_handover_barrier, src/color_barrier.h). */
        ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
         ps5vk_colour_readback_handover_barrier(b));
    /* The linear staging image the pinned draw module reads back through gets
     * exactly the two transitions that module records
     * (vktDrawImageObjectUtil.cpp:415-443): UNDEFINED to GENERAL for the
     * transfer write that fills it, and GENERAL to GENERAL from that transfer
     * write to the host read that consumes it. Both layouts are already
     * meaningful for this role and the stage/access scopes are checked by the
     * generic command-scope gate; anything else stays refused. */
    if(ps5vk_linear_staging_image(image))return
        (b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
         b->newLayout==VK_IMAGE_LAYOUT_GENERAL &&
         !b->srcAccessMask && b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT) ||
        (b->oldLayout==VK_IMAGE_LAYOUT_GENERAL &&
         b->newLayout==VK_IMAGE_LAYOUT_GENERAL &&
         b->srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT &&
         b->dstAccessMask==VK_ACCESS_HOST_READ_BIT);
    if(ps5vk_storage_image(image))return
        b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED &&
        b->newLayout==VK_IMAGE_LAYOUT_GENERAL && !b->srcAccessMask &&
        b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT;
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
