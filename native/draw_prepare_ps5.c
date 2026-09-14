#include "draw_prepare_ps5.h"
#include "vertex_fetch.h"
#include "vertex_descriptor.h"
#include "texture_descriptor.h"
#include <string.h>
void ps5vk_native_release_draw(struct ps5vk_prepared_draw *draw)
{
    if (!draw) return;
    if (draw->backing) draw->memory.release(draw->memory.context, draw->backing);
    memset(draw, 0, sizeof(*draw));
}
static VkResult prepare_draw(VkDevice d, const struct ps5vk_operation *op, const VkRect2D *area,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],const struct ps5vk_vertex_fetch *vertex,
    uint64_t shader_address,const uint32_t *texture_words,struct ps5vk_prepared_draw *out)
{
    if (!out || out->backing || out->state) return VK_ERROR_UNKNOWN;
    if (!d || !op || (op->type != PS5VK_DRAW && op->type != PS5VK_DRAW_INDEXED) || !op->pipeline || op->pipeline->device != d ||
        !op->framebuffer || op->framebuffer->device != d || !op->render_pass || op->render_pass->device != d ||
        !d->memory.allocate || !d->memory.release || !d->memory.flush) return VK_ERROR_UNKNOWN;
    VkFramebuffer fb = op->framebuffer;
    if (fb->color_attachment >= fb->attachment_count || fb->attachment_count > 2 ||
        (fb->depth_attachment != VK_ATTACHMENT_UNUSED && fb->depth_attachment >= fb->attachment_count))
        return VK_ERROR_UNKNOWN;
    struct ps5vk_target_registers color, depth;
    VkResult rc = ps5vk_native_target(d, fb->attachments[fb->color_attachment], defaults, &color);
    if (rc != VK_SUCCESS) return rc;
    int has_depth = fb->depth_attachment != VK_ATTACHMENT_UNUSED;
    if (has_depth) {
        rc = ps5vk_native_target(d, fb->attachments[fb->depth_attachment], NULL, &depth);
        if (rc != VK_SUCCESS) return rc;
    }
    struct ps5vk_draw_state plan;
    rc = ps5vk_native_draw_state(op->pipeline, &op->viewport, &op->scissor,
        &color, has_depth ? &depth : NULL, area,
        fb->width, fb->height, &plan);
    if (rc != VK_SUCCESS) return rc;
    struct ps5vk_prepared_draw result = {.memory = d->memory, .bytes = sizeof(plan)};
    size_t table_offset=(sizeof(plan)+15u)&~(size_t)15u;
    if(vertex)result.bytes=table_offset+16;
    size_t texture_offset=result.bytes;
    if(texture_words)result.bytes+=48;
    size_t push_offset=result.bytes;
    if(plan.runtime.push_constant_size) {
        if(op->push_constant_size!=plan.runtime.push_constant_size)return VK_ERROR_UNKNOWN;
        result.bytes+=plan.runtime.push_constant_size;
    } else if(op->push_constant_size)return VK_ERROR_UNKNOWN;
    size_t bounce_offset=0;
    if(vertex && vertex->address && (uintptr_t)vertex->address%4) {
        bounce_offset=(result.bytes+3u)&~(size_t)3u;
        if(vertex->bytes>SIZE_MAX-bounce_offset)return VK_ERROR_OUT_OF_HOST_MEMORY;
        result.bytes=bounce_offset+(size_t)vertex->bytes;
    }
    void *address = NULL;
    rc = result.memory.allocate(result.memory.context, result.bytes, &address, &result.backing);
    if (rc != VK_SUCCESS) return rc;
    if (!address || !result.backing || (uintptr_t)address % 8 ||
        (uintptr_t)address >= (UINT64_C(1) << 48) ||
        result.bytes > (UINT64_C(1) << 48) - (uintptr_t)address) {
        ps5vk_native_release_draw(&result); return VK_ERROR_MEMORY_MAP_FAILED;
    }
    result.state = address; memcpy(result.state, &plan, sizeof(plan));
    if(plan.runtime.push_constant_size) {
        void *push=(unsigned char *)address+push_offset;
        if(((uintptr_t)push>>32)!=2) {
            ps5vk_native_release_draw(&result);return VK_ERROR_MEMORY_MAP_FAILED;
        }
        memcpy(push,op->push_constants,plan.runtime.push_constant_size);
        result.state->push_constant_low=(uint32_t)(uintptr_t)push;
    }
    if(vertex) {
        uint32_t *table=(uint32_t *)((unsigned char *)address+table_offset);
        if(!ps5vk_vertex_table_address(shader_address,(uintptr_t)table,16)) {
            ps5vk_native_release_draw(&result);return VK_ERROR_MEMORY_MAP_FAILED;
        }
        uint32_t vertex_words[4]={0};
        if(vertex->address) {
            const void *source=vertex->address;
            if((uintptr_t)source%4) {
                void *bounce=(unsigned char *)address+bounce_offset;
                memcpy(bounce,source,(size_t)vertex->bytes);source=bounce;
                result.vertex_bounce=bounce;result.vertex_bounce_bytes=(size_t)vertex->bytes;
            }
            if(ps5vk_vertex_descriptor(vertex_words,(uintptr_t)source,vertex->bytes,
                vertex->stride,vertex->attribute_extent)) {
                ps5vk_native_release_draw(&result);return VK_ERROR_MEMORY_MAP_FAILED;
            }
        }
        memcpy(table,vertex_words,16);result.vertex_table=table;
    }
    if(texture_words) {
        uint32_t *table=(uint32_t *)((unsigned char *)address+texture_offset);
        if(!ps5vk_vertex_table_address(shader_address,(uintptr_t)table,48)) {
            ps5vk_native_release_draw(&result);return VK_ERROR_MEMORY_MAP_FAILED;
        }
        memcpy(table,texture_words,48);result.texture_table=table;
    }
    rc = result.memory.flush(result.memory.context, result.backing, 0, result.bytes);
    if (rc != VK_SUCCESS) { ps5vk_native_release_draw(&result); return rc; }
    *out = result; return VK_SUCCESS;
}
VkResult ps5vk_native_prepare_draw(VkDevice d,const struct ps5vk_operation *op,const VkRect2D *area,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],struct ps5vk_prepared_draw *out)
{ return prepare_draw(d,op,area,defaults,NULL,0,NULL,out); }

VkResult ps5vk_native_prepare_vertex_draw(VkDevice d,const struct ps5vk_operation *op,const VkRect2D *area,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],const struct ps5vk_graphics_key *key,
    uint64_t shader_address,struct ps5vk_prepared_draw *out)
{
    if(!out || out->backing || out->state)return VK_ERROR_UNKNOWN;
    struct ps5vk_vertex_fetch fetch;
    VkResult rc=ps5vk_vertex_fetch_span(d,key,op,&fetch);if(rc!=VK_SUCCESS)return rc;
    uint32_t texture[12];const uint32_t *texture_words=NULL;
    if(op->pipeline && op->pipeline->set_count) {
        VkDescriptorSet set=op->sets[0];
        if(op->pipeline->set_count!=1 || !set || set->pool->device!=d || set->generation!=op->generations[0] ||
            set->signature.count!=1 || set->signature.binding[0].count!=1 ||
            set->signature.type[0]!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || !set->defined[0])return VK_ERROR_UNKNOWN;
        rc=ps5vk_texture_descriptor(d,set->images[0].imageView,set->images[0].sampler,texture);
        if(rc!=VK_SUCCESS)return rc;
        texture_words=texture;
    }
    return prepare_draw(d,op,area,defaults,&fetch,shader_address,texture_words,out);
}
