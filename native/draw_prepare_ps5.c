#include "draw_prepare_ps5.h"
#include "vertex_fetch.h"
#include "vertex_descriptor.h"
#include "texture_descriptor.h"
#include "descriptor_table_layout.h"
#include "descriptor_encode.h"
#include "graphics_pipeline_ps5.h"
#include "runtime_resource_use.h"
#include "graphics_descriptor_profile.h"
#include <string.h>

static int graphics_descriptor_type(VkDescriptorType type)
{
    return ps5vk_graphics_descriptor_type(type);
}

static int graphics_buffer_type(VkDescriptorType type)
{
    return ps5vk_graphics_buffer_type(type);
}

static VkResult descriptor_plan(VkDevice d,const struct ps5vk_operation *op,
    const struct ps5vk_runtime_draw_abi *runtime,const struct ps5vk_runtime_draw_abi *hull,
    struct ps5vk_descriptor_table_layout *tables,
    uint32_t *mask)
{
    VkPipeline p=op->pipeline;
    VkResult rc=ps5vk_descriptor_table_layout_build(p->set_count,p->sets,tables);
    if(rc!=VK_SUCCESS)return rc;
    /* The visibility a binding may name, exactly as the compiler profile decides
     * it: the two standalone stages, plus the geometry stage when the pipeline's
     * pre-raster program is the merged vertex+geometry pair (the pinned
     * conformance module binds its uniform buffer and its sampled image to that
     * stage alone). A binding the pipeline would not execute is still refused,
     * and the flag comes from the built pair rather than from a second reading
     * of the compiler metadata, which no longer exists at draw time. */
    VkShaderStageFlags visible=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
    {
        const struct ps5vk_native_graphics_pipeline *native=
            p->graphics_state?(const struct ps5vk_native_graphics_pipeline *)p->graphics_state:NULL;
        if(native && native->pair && native->pair->ready && native->pair->geometry_preraster)
            visible|=VK_SHADER_STAGE_GEOMETRY_BIT;
        /* And the tessellation pair's two stages when the pipeline carries
         * it, for the same reason and from the same source.
         *
         * They were missing, and the two profiles disagreed because of it:
         * descriptor_profile_supported() admits a binding named for a
         * tessellation stage whenever the pipeline has tessellation, so such a
         * pipeline is CREATED, and this mask then refused it at submit with
         * VK_ERROR_FEATURE_NOT_PRESENT. A descriptor bound to either
         * tessellation stage - a uniform buffer as much as anything else -
         * could therefore never be drawn with, and the failure appeared only
         * at vkQueueSubmit, which is the worst place to learn it. */
        if(native && native->pair && native->pair->ready && native->pair->tessellation)
            visible|=VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT|
                     VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    }
    *mask=0;
    if(runtime->enabled) {
        for(unsigned s=0;s<PS5VK_MAX_SETS;++s) {
            /* One canonical table per set, shared by every using stage. */
            if(ps5vk_runtime_resource_bindings(runtime,hull,s))*mask|=1u<<s;
        }
    } else if(p->set_count) {
        if(p->set_count!=1 || p->sets[0].count!=1 || p->sets[0].binding[0].count!=1)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        *mask=1;
    }
    for(unsigned s=0;s<PS5VK_MAX_SETS;++s)if(*mask&(1u<<s)) {
        VkDescriptorSet set=op->sets[s];
        if(s>=p->set_count || !tables->set_bytes[s] || !set || !set->pool ||
           set->pool->device!=d || set->generation!=op->generations[s] ||
           memcmp(&set->signature,&p->sets[s],sizeof(set->signature)))return VK_ERROR_UNKNOWN;
        for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
            const struct ps5vk_binding *binding=&set->signature.binding[b];
            if(!binding->count)continue;
            if(!graphics_descriptor_type(set->signature.type[b]) ||
               !(binding->stages&visible))
                return VK_ERROR_FEATURE_NOT_PRESENT;
            /* An input attachment is fragment-visible resource-only image data.
             * vkCreateDescriptorSetLayout admits only the fragment stage for
             * it, so anything else here is a signature this path cannot
             * deliver and must refuse rather than encode for the wrong stage. */
            if(set->signature.type[b]==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT &&
               binding->stages!=VK_SHADER_STAGE_FRAGMENT_BIT)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            /* A binding no compiled stage dereferences is a declaration, not a
             * requirement: it needs no defined descriptor. A set the compiler
             * could not name (zero mask on both stages) keeps its whole
             * declaration so an unknown entry can never look unused. */
            const uint64_t used=ps5vk_runtime_resource_bindings(runtime,hull,s);
            if(runtime->enabled && used && !(used&(UINT64_C(1)<<b)))continue;
            for(unsigned e=0;e<binding->count;++e) {
                unsigned index=binding->first+e;
                if(!set->defined[index])return VK_ERROR_UNKNOWN;
                /* A null buffer never reaches the encoder. Ownership and the
                 * resolved span are validated by ps5vk_buffer_descriptor,
                 * which is the only place that can read a buffer handle. */
                if(graphics_buffer_type(set->signature.type[b]) &&
                   !set->buffers[index].buffer)return VK_ERROR_UNKNOWN;
                /* The recorded view and the layout it is consumed through are
                 * preconditions this path owns: the encoder receives a view
                 * and cannot see a VkDescriptorImageInfo. VkDescriptorImageInfo's
                 * sampler member is IGNORED for an input attachment, so it is
                 * deliberately neither read nor rejected here or below. */
                if(set->signature.type[b]==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
                    const VkDescriptorImageInfo *image=&set->images[index];
                    if(!image->imageView ||
                       (image->imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                        image->imageLayout!=VK_IMAGE_LAYOUT_GENERAL))return VK_ERROR_UNKNOWN;
                }
            }
        }
    }
    return VK_SUCCESS;
}
void ps5vk_native_release_draw(struct ps5vk_prepared_draw *draw)
{
    if (!draw) return;
    if (draw->backing) draw->memory.release(draw->memory.context, draw->backing);
    memset(draw, 0, sizeof(*draw));
}
static VkResult prepare_draw(VkDevice d, const struct ps5vk_operation *op, const VkRect2D *area,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],const struct ps5vk_vertex_fetch_table *vertices,
    uint64_t shader_address,struct ps5vk_prepared_draw *out)
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
    /* The index width the draw's bound index buffer declares, or zero for a
     * non-indexed draw. The direct and indirect indexed ops both carry the
     * binding vkCmdBindIndexBuffer recorded, and the hardware needs the width to
     * compare the right reset value. */
    const unsigned index_width =
        (op->type == PS5VK_DRAW_INDEXED || op->type == PS5VK_DRAW_INDEXED_INDIRECT) ?
        (op->indices.type == VK_INDEX_TYPE_UINT16 ? 2u : 4u) : 0u;
    rc = ps5vk_native_draw_state(op->pipeline, &op->viewport, &op->scissor,
        &color, has_depth ? &depth : NULL, area,
        fb->width, fb->height, index_width, &plan);
    if (rc != VK_SUCCESS) return rc;
    struct ps5vk_descriptor_table_layout tables;
    uint32_t set_mask=0;
    rc=descriptor_plan(d,op,&plan.runtime,&plan.hull_runtime,&tables,&set_mask);
    if(rc!=VK_SUCCESS)return rc;
    struct ps5vk_prepared_draw result = {.memory = d->memory, .bytes = sizeof(plan)};
    size_t table_offset=(sizeof(plan)+15u)&~(size_t)15u;
    if(vertices && (!vertices->count || vertices->count>PS5VK_MAX_VERTEX_BINDINGS))return VK_ERROR_UNKNOWN;
    if(vertices)result.bytes=table_offset+16u*vertices->count;
    size_t descriptor_offsets[PS5VK_MAX_SETS]={0};
    for(unsigned s=0;s<PS5VK_MAX_SETS;++s)if(set_mask&(1u<<s)) {
        result.bytes=(result.bytes+15u)&~(size_t)15u;
        descriptor_offsets[s]=result.bytes;
        result.descriptor_bytes[s]=tables.set_bytes[s];
        result.bytes+=tables.set_bytes[s];
    }
    size_t push_offset=result.bytes;
    uint32_t push_bytes=0;
    if(ps5vk_runtime_push_bytes(&plan.runtime,&plan.hull_runtime,
            op->push_constant_size,&push_bytes))return VK_ERROR_UNKNOWN;
    result.bytes+=push_bytes;
    size_t bounce_offsets[PS5VK_MAX_VERTEX_BINDINGS]={0};
    for(uint32_t i=0;vertices && i<vertices->count;++i) {
        const struct ps5vk_vertex_fetch *vertex=&vertices->bindings[i];
        if(!vertex->address || !((uintptr_t)vertex->address%4))continue;
        if(result.bytes>SIZE_MAX-3u)return VK_ERROR_OUT_OF_HOST_MEMORY;
        bounce_offsets[i]=(result.bytes+3u)&~(size_t)3u;
        if(vertex->bytes>SIZE_MAX-bounce_offsets[i])return VK_ERROR_OUT_OF_HOST_MEMORY;
        result.bytes=bounce_offsets[i]+(size_t)vertex->bytes;
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
    if(push_bytes) {
        void *push=(unsigned char *)address+push_offset;
        if(((uintptr_t)push>>32)!=2) {
            ps5vk_native_release_draw(&result);return VK_ERROR_MEMORY_MAP_FAILED;
        }
        memcpy(push,op->push_constants,push_bytes);
        result.state->push_constant_low=(uint32_t)(uintptr_t)push;
    }
    if(vertices) {
        uint32_t *table=(uint32_t *)((unsigned char *)address+table_offset);
        if(!ps5vk_vertex_table_address(shader_address,(uintptr_t)table,16u*vertices->count)) {
            ps5vk_native_release_draw(&result);return VK_ERROR_MEMORY_MAP_FAILED;
        }
        for(uint32_t i=0;i<vertices->count;++i) {
        const struct ps5vk_vertex_fetch *vertex=&vertices->bindings[i];
        uint32_t vertex_words[4]={0};
        if(vertex->address) {
            const void *source=vertex->address;
            if((uintptr_t)source%4) {
                void *bounce=(unsigned char *)address+bounce_offsets[i];
                memcpy(bounce,source,(size_t)vertex->bytes);source=bounce;
                if(!result.vertex_bounce)result.vertex_bounce=bounce;
                result.vertex_bounce_bytes+=(size_t)vertex->bytes;
            }
            if(ps5vk_vertex_descriptor(vertex_words,(uintptr_t)source,vertex->bytes,
                vertex->stride,vertex->attribute_extent)) {
                ps5vk_native_release_draw(&result);return VK_ERROR_MEMORY_MAP_FAILED;
            }
        }
        memcpy(table+4u*i,vertex_words,16);
        }
        result.vertex_table=table;
    }
    for(unsigned s=0;s<PS5VK_MAX_SETS;++s)if(set_mask&(1u<<s)) {
        uint32_t *table=(uint32_t *)((unsigned char *)address+descriptor_offsets[s]);
        if(!ps5vk_vertex_table_address(shader_address,(uintptr_t)table,tables.set_bytes[s])) {
            ps5vk_native_release_draw(&result);return VK_ERROR_MEMORY_MAP_FAILED;
        }
        memset(table,0,tables.set_bytes[s]);
        VkDescriptorSet set=op->sets[s];
        for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
            const struct ps5vk_binding *binding=&set->signature.binding[b];
            /* An unused declared binding keeps its zeroed record and is never
             * encoded, so it cannot require a view, a sampler or a descriptor. */
            if(plan.runtime.enabled) {
                const uint64_t used=ps5vk_runtime_resource_bindings(
                    &plan.runtime,&plan.hull_runtime,s);
                if(used && binding->count && !(used&(UINT64_C(1)<<b)))continue;
            }
            for(unsigned e=0;e<binding->count;++e) {
                unsigned index=binding->first+e;
                uint32_t *words=table+(tables.binding[s][b].byte_offset+
                    e*tables.binding[s][b].byte_stride)/4;
                if(graphics_buffer_type(set->signature.type[b])) {
                    VkDeviceSize dynamic=ps5vk_dynamic_descriptor_type(
                        set->signature.type[b])?op->graphics_dynamic_offsets[s][index]:0;
                    rc=ps5vk_buffer_descriptor(d,&set->buffers[index],dynamic,words);
                } else if(set->signature.type[b]==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
                    /* Resource-only image data: the encoder writes the eight
                     * DWORD image record and nothing else, and it is never the
                     * combined T#/S# path - an input attachment's slot in the
                     * table can therefore never receive sampler words. */
                    rc=ps5vk_image_resource_descriptor(d,set->images[index].imageView,words);
                } else {
                    const VkDescriptorImageInfo *image=&set->images[index];
                    rc=ps5vk_texture_descriptor(d,image->imageView,image->sampler,words);
                }
                if(rc!=VK_SUCCESS){ps5vk_native_release_draw(&result);return rc;}
            }
        }
        result.descriptor_tables[s]=table;
    }
    result.texture_table=result.descriptor_tables[0]; /* precompiled single-table ABI */
    rc = result.memory.flush(result.memory.context, result.backing, 0, result.bytes);
    if (rc != VK_SUCCESS) { ps5vk_native_release_draw(&result); return rc; }
    *out = result; return VK_SUCCESS;
}
VkResult ps5vk_native_prepare_draw(VkDevice d,const struct ps5vk_operation *op,const VkRect2D *area,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],struct ps5vk_prepared_draw *out)
{ return prepare_draw(d,op,area,defaults,NULL,0,out); }

VkResult ps5vk_native_prepare_resource_draw(VkDevice d,const struct ps5vk_operation *op,const VkRect2D *area,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],uint64_t shader_address,
    struct ps5vk_prepared_draw *out)
{ return prepare_draw(d,op,area,defaults,NULL,shader_address,out); }

VkResult ps5vk_native_prepare_vertex_draw(VkDevice d,const struct ps5vk_operation *op,const VkRect2D *area,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],const struct ps5vk_graphics_key *key,
    uint64_t shader_address,struct ps5vk_prepared_draw *out)
{
    if(key && (key->vertex_binding_count!=1 || !key->vertex_bindings ||
               key->vertex_bindings[0].binding!=0))return VK_ERROR_FEATURE_NOT_PRESENT;
    return ps5vk_native_prepare_vertex_draw_masked(d,op,area,defaults,key,shader_address,1,out);
}
VkResult ps5vk_native_prepare_vertex_draw_masked(VkDevice d,const struct ps5vk_operation *op,const VkRect2D *area,
    const ps5_agc_register defaults[PS5_COLOR_REGISTER_COUNT],const struct ps5vk_graphics_key *key,
    uint64_t shader_address,uint32_t usage_mask,struct ps5vk_prepared_draw *out)
{
    if(!out || out->backing || out->state)return VK_ERROR_UNKNOWN;
    struct ps5vk_vertex_fetch_table sparse,fetch;
    VkResult rc=ps5vk_vertex_fetch_used_spans(d,key,op,usage_mask,&sparse);if(rc!=VK_SUCCESS)return rc;
    rc=ps5vk_vertex_fetch_compact(&sparse,usage_mask,&fetch);if(rc!=VK_SUCCESS)return rc;
    return prepare_draw(d,op,area,defaults,&fetch,shader_address,out);
}
