#include "graphics_program.h"
#include <string.h>
static int valid_sets(const struct ps5vk_graphics_key *k)
{
    if(k->descriptor_set_count>PS5VK_MAX_SETS || (k->descriptor_set_count && !k->descriptor_sets))return 0;
    for(unsigned s=0;s<k->descriptor_set_count;++s) {
        uint32_t count=0;
        for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
            const struct ps5vk_binding *binding=&k->descriptor_sets[s].binding[b];
            if(binding->count>PS5VK_MAX_DESCRIPTORS-count || binding->first!=count)return 0;
            count+=binding->count;
        }
        if(count!=k->descriptor_sets[s].count)return 0;
    }
    return 1;
}
static int equal_sets(const struct ps5vk_graphics_key *a,const struct ps5vk_graphics_key *b)
{
    if(a->descriptor_set_count!=b->descriptor_set_count)return 0;
    for(unsigned s=0;s<a->descriptor_set_count;++s)for(unsigned i=0;i<PS5VK_MAX_BINDINGS;++i) {
        const struct ps5vk_binding *x=&a->descriptor_sets[s].binding[i],*y=&b->descriptor_sets[s].binding[i];
        if(x->count!=y->count || (x->count && (x->stages!=y->stages ||
            a->descriptor_sets[s].type[i]!=b->descriptor_sets[s].type[i])))return 0;
    }
    return 1;
}
static int valid_vertex_layout(const struct ps5vk_graphics_key *k)
{
    if(k->vertex_binding_count>16 || k->vertex_attribute_count>32 ||
       (k->vertex_binding_count && !k->vertex_bindings) ||
       (k->vertex_attribute_count && !k->vertex_attributes))return 0;
    for(uint32_t i=0;i<k->vertex_binding_count;++i) {
        const VkVertexInputBindingDescription *b=&k->vertex_bindings[i];
        if(b->inputRate!=VK_VERTEX_INPUT_RATE_VERTEX && b->inputRate!=VK_VERTEX_INPUT_RATE_INSTANCE)return 0;
        for(uint32_t j=0;j<i;++j)if(k->vertex_bindings[j].binding==b->binding)return 0;
    }
    for(uint32_t i=0;i<k->vertex_attribute_count;++i) {
        const VkVertexInputAttributeDescription *a=&k->vertex_attributes[i];
        unsigned found=0;
        if(a->format==VK_FORMAT_UNDEFINED)return 0;
        for(uint32_t j=0;j<k->vertex_binding_count;++j)found|=k->vertex_bindings[j].binding==a->binding;
        if(!found)return 0;
        for(uint32_t j=0;j<i;++j)if(k->vertex_attributes[j].location==a->location)return 0;
    }
    return 1;
}
/* Vulkan descriptions are keyed by binding/location, not array order or
 * padding bytes. Do not silently conflate stride, format, offset or rate. */
static int equal_vertex_layout(const struct ps5vk_graphics_key *a,const struct ps5vk_graphics_key *b)
{
    if(a->vertex_binding_count!=b->vertex_binding_count || a->vertex_attribute_count!=b->vertex_attribute_count)return 0;
    for(uint32_t i=0;i<a->vertex_binding_count;++i) {
        const VkVertexInputBindingDescription *x=&a->vertex_bindings[i];unsigned found=0;
        for(uint32_t j=0;j<b->vertex_binding_count;++j) {
            const VkVertexInputBindingDescription *y=&b->vertex_bindings[j];
            found|=x->binding==y->binding && x->stride==y->stride && x->inputRate==y->inputRate;
        }
        if(!found)return 0;
    }
    for(uint32_t i=0;i<a->vertex_attribute_count;++i) {
        const VkVertexInputAttributeDescription *x=&a->vertex_attributes[i];unsigned found=0;
        for(uint32_t j=0;j<b->vertex_attribute_count;++j) {
            const VkVertexInputAttributeDescription *y=&b->vertex_attributes[j];
            found|=x->location==y->location && x->binding==y->binding && x->format==y->format && x->offset==y->offset;
        }
        if(!found)return 0;
    }
    return 1;
}
static int equal_module(const struct ps5vk_graphics_module_key *a,
                         const struct ps5vk_graphics_module_key *b)
{
    return a->words && b->words && a->entry && b->entry &&
        a->word_count >= 5 && a->word_count <= 4 * 1024 * 1024 &&
        a->word_count == b->word_count && !strcmp(a->entry, b->entry) &&
        a->specialization_count==b->specialization_count &&
        !memcmp(a->specializations,b->specializations,
                a->specialization_count*sizeof(*a->specializations)) &&
        !memcmp(a->words, b->words, a->word_count * sizeof(uint32_t));
}
VkResult ps5vk_graphics_resolve(const struct ps5vk_graphics_library *library,
    const struct ps5vk_graphics_key *key, const struct ps5vk_graphics_program **out)
{
    if (!out) return VK_ERROR_UNKNOWN;
    *out = NULL;
    if (!library || !key || (library->count && !library->programs)) return VK_ERROR_UNKNOWN;
    if (!valid_sets(key))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!valid_vertex_layout(key)) return VK_ERROR_UNKNOWN;
    for (size_t i = 0; i < library->count; ++i) {
        const struct ps5vk_graphics_program *program = &library->programs[i];
        const struct ps5vk_graphics_key *p = &program->key;
        if (!program->backend_data || !valid_sets(p) || !equal_sets(p,key) || !valid_vertex_layout(p) || !equal_vertex_layout(p,key) ||
            p->topology != key->topology || p->color_format != key->color_format || p->samples != key->samples ||
            p->color_write_mask != key->color_write_mask || p->blend_enable != key->blend_enable ||
            (key->blend_enable && (
                p->src_color_blend_factor != key->src_color_blend_factor ||
                p->dst_color_blend_factor != key->dst_color_blend_factor ||
                p->color_blend_op != key->color_blend_op ||
                p->src_alpha_blend_factor != key->src_alpha_blend_factor ||
                p->dst_alpha_blend_factor != key->dst_alpha_blend_factor ||
                p->alpha_blend_op != key->alpha_blend_op ||
                memcmp(p->blend_constants,key->blend_constants,sizeof(p->blend_constants)))) ||
            p->push_constant_size!=key->push_constant_size ||
            memcmp(p->push_constant_stages,key->push_constant_stages,
                   sizeof(p->push_constant_stages)) ||
            !equal_module(&p->vertex, &key->vertex) || !equal_module(&p->fragment, &key->fragment)) continue;
        /* A record with a geometry stage matches only a key with the same
         * geometry module: a two-stage program must never satisfy a three-stage
         * pipeline by accident, and vice versa. */
        if ((p->geometry.words!=NULL) != (key->geometry.words!=NULL)) continue;
        if (key->geometry.words && !equal_module(&p->geometry, &key->geometry)) continue;
        /* The tessellation pair is part of the identity for the same reason,
         * and it carries one extra input: the patch control points the control
         * stage's output vertex count and the evaluation stage's input arrays
         * are derived from. A record whose patches were compiled for a
         * different count is a different program. */
        const int p_tess=p->tess_control.words!=NULL, key_tess=key->tess_control.words!=NULL;
        if (p_tess!=key_tess || p->patch_control_points!=key->patch_control_points) continue;
        if (key_tess) {
            if (!equal_module(&p->tess_control, &key->tess_control) ||
                !equal_module(&p->tess_eval, &key->tess_eval)) continue;
        } else if (p->tess_eval.words || key->tess_eval.words) continue;
        /* Multiple matching records are an ambiguous compiler library, not
         * permission to choose the first potentially different backend object. */
        if (*out) { *out = NULL; return VK_ERROR_UNKNOWN; }
        *out = program;
    }
    return *out ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT;
}
