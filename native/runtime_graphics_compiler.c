/*
 * Copyright (C) 2026 BlackBearReloaded
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The GFX1013/PSBC vertex format mapping is adapted from the vertex-format
 * contract in BlackBearReloaded's ps5-opengl, src/gallium/ps5/ps5_screen.c at
 * commit 7f9bfabdddb187a11e4401058eba8c9e55194d0a (GPL-3.0-or-later).
 */
#include "runtime_graphics_compiler.h"
#include "runtime_resource_use.h"
#include "spirv_graphics_interface.h"
#include "descriptor_table_layout.h"
#include "graphics_descriptor_profile.h"
#include "blend_ps5.h"
#include "color_attachment_contract.h"
#include <stdlib.h>
#include <string.h>

/* Structural/entry screening only; PSBC is responsible for shader validity. */
static int module_supported(const struct ps5vk_graphics_module_key *m,unsigned model)
{
    if(!m->words || m->word_count<5 || m->word_count>4u*1024u*1024u || !m->entry ||
        !*m->entry || strlen(m->entry)>=64 || m->words[0]!=0x07230203u)return 0;
    unsigned entries=0,functions=0,in_function=0;
    for(size_t at=5;at<m->word_count;) {
        unsigned n=m->words[at]>>16,op=m->words[at]&65535u;
        if(!n || n>m->word_count-at)return 0;
        const uint32_t *w=m->words+at;
        if(op==15) {
            if(n<4)return 0;
            const char *name=(const char *)(w+3);
            if(!memchr(name,0,(n-3u)*4u))return 0;
            if(w[1]==model && !strcmp(name,m->entry))++entries;
        }
        if(op==59) {
            if(n<4)return 0;
            /* Sampled images (UniformConstant) and uniform blocks (Uniform) are
             * admitted only through the separately checked descriptor profile,
             * which now delivers both record kinds from one table. Storage
             * buffers stay outside this bounded graphics compiler. */
            if(w[3]==12)return 0;
        }
        if(op==54) {
            if(n!=5 || in_function)return 0;
            in_function=1;++functions;
        } else if(op==56) {
            if(n!=1 || !in_function)return 0;
            in_function=0;
        }
        at+=n;
    }
    return entries==1 && functions && !in_function;
}

void ps5vk_runtime_graphics_free(void *context,const void *data)
{
    (void)context;
    struct ps5vk_runtime_graphics_program *p=(void *)data;
    if(!p)return;
    psbc_free_output(&p->vertex);psbc_free_output(&p->fragment);
    psbc_free_output(&p->hull);psbc_free_output(&p->domain);
    psbc_free_output(&p->domain_legacy);free(p);
}

#if (defined(PS5VK_TESS_LEGACY_DOMAIN) && PS5VK_TESS_LEGACY_DOMAIN) || \
    (defined(PS5VK_TESS_LEGACY_VS_CONTROL) && PS5VK_TESS_LEGACY_VS_CONTROL)
/* DIAGNOSTIC: the draw ABI of a pre-raster program compiled as a LEGACY
 * hardware VS. ps5vk_runtime_draw_abi_build cannot produce it - that builder
 * is NGG-only by contract (NGG hardware stage, NGG LDS layout) - so it is
 * derived from the NGG program's VALIDATED ABI with every program-specific
 * field replaced by the legacy program's: its user-SGPR count, a zero window
 * base (a hardware VS has no system-SGPR preamble), no merged-pair system
 * registers, its own draw-parameter, vertex-buffer, ring-table and
 * push/descriptor slots, and the LDS-layout word - meaningless to a legacy
 * program - parked in the first user dword nothing else names, so the value
 * builder's collision check stays honest. The result is run through the same
 * value builder the NGG ABI passed. Returns 0 on success. */
static int derive_legacy_abi(const struct ps5vk_runtime_draw_abi *ngg,
    const PsbcShaderMetadata *lm,struct ps5vk_runtime_draw_abi *out)
{
    if(lm->hardware_stage!=PSBC_HW_STAGE_VERTEX || !lm->linkage_valid ||
       !lm->user_sgpr_count || lm->user_sgpr_count>16 ||
       lm->user_data_window_base)return -1;
    struct ps5vk_runtime_draw_abi legacy=*ngg;
    legacy.vertex_count=lm->user_sgpr_count;
    legacy.window_base=0;
    legacy.esgs_described=0;
    legacy.esgs_gs_tg_info_sgpr=legacy.esgs_merged_wave_info_sgpr=0;
    legacy.base_vertex_slot=lm->base_vertex_valid?lm->base_vertex_user_data_dword:UINT32_MAX;
    legacy.start_instance_slot=lm->start_instance_valid?lm->start_instance_user_data_dword:UINT32_MAX;
    legacy.draw_id_slot=lm->draw_id_valid?lm->draw_id_user_data_dword:UINT32_MAX;
    legacy.view_index_slot=lm->view_index_valid?lm->view_index_user_data_dword:UINT32_MAX;
    legacy.vertex_buffer_valid=lm->vertex_buffer_table_valid;
    legacy.vertex_buffer_slot=lm->vertex_buffer_table_user_data_dword;
    legacy.vertex_buffer_usage_mask=lm->vertex_buffer_usage_mask;
    legacy.ring_table_valid=lm->ps5_ring_table_valid;
    legacy.ring_table_slot=lm->ps5_ring_table_valid?lm->ps5_ring_table_user_data_dword:0u;
    legacy.ring_table_low=legacy.ring_table_high=0;
    legacy.vertex_push_slot=lm->push_constants_valid?lm->push_constants_user_data_dword:UINT32_MAX;
    legacy.legacy_vs=1;
    uint32_t tables[PS5VK_RUNTIME_DESCRIPTOR_SETS]={0};
    for(unsigned s=0;s<PS5VK_RUNTIME_DESCRIPTOR_SETS;++s) {
        legacy.vertex_descriptor_valid[s]=lm->descriptor_set_valid[s];
        legacy.vertex_descriptor_slot[s]=lm->descriptor_set_user_data_dword[s];
        legacy.vertex_used_bindings[s]=lm->descriptor_used_binding_mask[s];
        if(legacy.vertex_descriptor_valid[s] || legacy.fragment_descriptor_valid[s])
            tables[s]=16*(s+1);
    }
    uint32_t lds_slot=UINT32_MAX;
    for(uint32_t d=0;d<legacy.vertex_count && lds_slot==UINT32_MAX;++d) {
        int used=(legacy.ring_table_valid &&
            (d==legacy.ring_table_slot || d==legacy.ring_table_slot+1u)) ||
            d==legacy.vertex_push_slot || d==legacy.base_vertex_slot ||
            d==legacy.start_instance_slot || d==legacy.draw_id_slot ||
            d==legacy.view_index_slot ||
            (legacy.vertex_buffer_valid && d==legacy.vertex_buffer_slot);
        for(unsigned s=0;s<PS5VK_RUNTIME_DESCRIPTOR_SETS;++s)
            used|=legacy.vertex_descriptor_valid[s] && d==legacy.vertex_descriptor_slot[s];
        if(!used)lds_slot=d;
    }
    if(lds_slot==UINT32_MAX)return -1;
    legacy.lds_slot=lds_slot;legacy.lds_value=0;
    uint32_t check_vertex[16],check_pixel[16];
    if(ps5vk_runtime_draw_values_sets(&legacy,0,0,0,0,legacy.vertex_buffer_valid?16:0,
           legacy.push_constant_size?4:0,tables,check_vertex,check_pixel))return -1;
    *out=legacy;
    return 0;
}
#endif

int ps5vk_runtime_graphics_distance_reads_described(const PsbcShaderMetadata *pre_raster,
    const PsbcShaderMetadata *fragment,unsigned declared_clip,unsigned declared_cull)
{
    if(!pre_raster || !fragment || declared_clip>8u || declared_cull>8u)return 0;
    if(!declared_clip && !declared_cull)return 1;
    /* The pixel stage's own report must agree with what the module declares. */
    if(fragment->ps_clip_distance_reads!=declared_clip ||
       fragment->ps_cull_distance_reads!=declared_cull)return 0;
    /* The pre-raster stage must export every component the pixel stage reads.
     * The masks are packed: the clip components occupy the low bits and the cull
     * components continue immediately after them, so the cull field starts at
     * the clip count. */
    unsigned producer_clip=0;
    for(uint32_t mask=pre_raster->clip_distance_mask;mask;mask>>=1)
        producer_clip+=mask&1u;
    if(producer_clip>8u || declared_cull>8u-producer_clip)return 0;
    if((pre_raster->clip_distance_mask&((1u<<declared_clip)-1u))!=
           ((1u<<declared_clip)-1u) ||
       (pre_raster->cull_distance_mask&(((1u<<declared_cull)-1u)<<producer_clip))!=
           (((1u<<declared_cull)-1u)<<producer_clip))return 0;
    /* ... and every register the pixel stage NAMES must exist on the export
     * side, exactly once: the producer word carries the register's parameter
     * index, the consumer word names the register it reads, and the AGC linker
     * pairs them on that private key. The pixel stage only names the registers
     * it actually reads - a shader that reads gl_ClipDistance[4] and nothing
     * else names the second register and not the first - so the producer may
     * describe registers the consumer does not mention, and the other way round
     * is what would be wrong. */
    const unsigned registers=(producer_clip+declared_cull+3u)/4u;
    if(!registers || registers>2u)return 0;
    unsigned named=0;
    for(uint32_t i=0;i<fragment->input_semantic_count;++i) {
        const uint32_t key=fragment->input_semantics[i]&255u;
        if(key<PSBC_SEMANTIC_DISTANCE_REGISTER ||
           key>=PSBC_SEMANTIC_DISTANCE_REGISTER+registers)continue;
        unsigned described_out=0;
        for(uint32_t j=0;j<pre_raster->output_semantic_count;++j)
            described_out+=(pre_raster->output_semantics[j]&255u)==key;
        if(described_out!=1)return 0;
        ++named;
    }
    return named!=0;
}

static int fragment_memory_write_state(const PsbcShaderMetadata *fragment)
{
    /* GFX10.3 DB_SHADER_CONTROL.  PSBC derives these two bits from
     * nir_shader::info.writes_memory: ordinary fragment shaders clear both,
     * while a fragment store/atomic needs both EXEC_ON_HIER_FAIL and
     * EXEC_ON_NOOP so helper/early-Z outcomes cannot suppress the side effect.
     * A torn pair is neither contract and is refused rather than guessed. */
    /* Pre-raster-only contract tests use an all-zero placeholder because they
     * are checking a linked stage before a pixel half exists.  That is the one
     * representation of "no fragment stage"; a real non-pixel or malformed
     * pixel record still fails closed. */
    if(!fragment->version && fragment->hardware_stage==PSBC_HW_STAGE_UNKNOWN)
        return 0;
    if(fragment->hardware_stage!=PSBC_HW_STAGE_PIXEL)return -1;
    for(uint32_t i=0;i<fragment->context_register_count;++i) {
        const PsbcRegisterWrite *reg=&fragment->context_registers[i];
        if(reg->offset!=0x203u)continue;
        const uint32_t execution=reg->value&UINT32_C(0x600);
        if(!execution)return 0;
        return execution==UINT32_C(0x600)?1:-1;
    }
    return -1;
}

int ps5vk_runtime_graphics_feature_use_ok(const PsbcShaderMetadata *pre_raster,
    const PsbcShaderMetadata *fragment,uint32_t feature_mask)
{
    if(!pre_raster || !fragment)return 0;
    const int fragment_writes=fragment_memory_write_state(fragment);
    if(fragment_writes<0)return 0;
#if PS5VK_OPTIONAL_STAGE_DIAGNOSTIC
    /* The witness builds exist to measure these capabilities before any of them
     * is advertised, so they skip the negotiation gate the shipping build
     * enforces on every acquisition. */
    (void)feature_mask;
    return 1;
#else
    if(pre_raster->clip_distance_mask &&
       !(feature_mask & PS5VK_FEATURE_SHADER_CLIP_DISTANCE))return 0;
    if(pre_raster->cull_distance_mask &&
       !(feature_mask & PS5VK_FEATURE_SHADER_CULL_DISTANCE))return 0;
    /* A merged pre-raster stage that reports the geometry source stage is a
     * geometry pipeline, and it needs the feature like any other stage. */
    if(pre_raster->source_stage==PSBC_STAGE_GEOMETRY &&
       !(feature_mask & PS5VK_FEATURE_GEOMETRY_SHADER))return 0;
    /* The hull half of a tessellation pipeline: its source stage names the
     * control half the compiler linked, so a tessellation pipeline can only be
     * compiled for a device that enabled the feature. The evaluation half is
     * gated by the TESS_EVAL check below when the caller passes it. */
    if(pre_raster->source_stage==PSBC_STAGE_TESS_CTRL &&
       !(feature_mask & PS5VK_FEATURE_TESSELLATION_SHADER))return 0;
    if(pre_raster->source_stage==PSBC_STAGE_TESS_EVAL &&
       !(feature_mask & PS5VK_FEATURE_TESSELLATION_SHADER))return 0;
    if(pre_raster->merged_geometry &&
       pre_raster->merged_es_source_stage==PSBC_STAGE_TESS_EVAL &&
       !(feature_mask & PS5VK_FEATURE_TESSELLATION_SHADER))return 0;
    if(fragment_writes &&
       !(feature_mask & PS5VK_FEATURE_FRAGMENT_STORES_AND_ATOMICS))return 0;
    return 1;
#endif
}

static int descriptor_profile_supported(const struct ps5vk_graphics_key *key)
{
    struct ps5vk_descriptor_table_layout tables;
    if (ps5vk_descriptor_table_layout_build(key->descriptor_set_count,
            key->descriptor_sets,&tables)!=VK_SUCCESS) return 0;
    if(tables.binding_count>PSBC_MAX_DESCRIPTOR_BINDINGS)return 0;
    /* Layout visibility does not require that a pipeline instantiate the
     * named stage. In particular, a shared layout can retain a TCS-only
     * binding in a VS+GS+FS pipeline. The canonical table validates stage
     * flags; descriptor options project bindings onto actual compile stages.
     * Actual shader accesses still need their visible binding. */
    for(unsigned s=0;s<key->descriptor_set_count;++s)
        for(unsigned b=0;b<PS5VK_MAX_BINDINGS;++b) {
            const struct ps5vk_set_signature *set=&key->descriptor_sets[s];
            /* The canonical table validates the full core visibility mask;
             * descriptor options project it onto the executing stages.
             * Combined image samplers coexist with the mandatory uniform-buffer
             * resources a real pipeline layout carries, and an input attachment
             * is admitted as fragment-only resource-only image data: it is read
             * by subpassLoad in a fragment shader, so a layout that exposes it
             * to the vertex stage is refused rather than projected onto a stage
             * that cannot read it. Every other descriptor type stays outside
             * the profile instead of being half-delivered. */
            if(set->binding[b].count &&
               !ps5vk_graphics_descriptor_type(set->type[b]))return 0;
            if(set->binding[b].count &&
               set->type[b]==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT &&
               set->binding[b].stages!=VK_SHADER_STAGE_FRAGMENT_BIT)return 0;
        }
    return 1;
}

/* The DI primitive type a patch-list draw feeds resolves in
 * graphics_program.h (pinned gfx103 DI_PT_PATCH); the compile options keep
 * the compiler's own default primitive state because the tessellator, not
 * the assembler, generates the rasterized primitive. */

/* Diagnostic rejection codes. When PS5VK_GEOMETRY_KEY_DIAG is defined (the SDK
 * build of a diagnostic CTS payload) a refused key is logged field by field, so
 * a console run names the condition and the shape instead of only reporting
 * VK_ERROR_FEATURE_NOT_PRESENT. The shipping build defines nothing and never
 * reads the code. */
unsigned ps5vk_runtime_graphics_diag_site;
int ps5vk_runtime_graphics_diag_result;
#if defined(PS5VK_GEOMETRY_KEY_DIAG) && PS5VK_GEOMETRY_KEY_DIAG
#include "ps5log.h"
#endif
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY == 4
#include "ps5log.h"
static uint64_t tess_code_fingerprint(const PsbcShaderOutput *shader)
{
    const unsigned char *bytes=shader->machine_code;
    uint64_t value=UINT64_C(14695981039346656037);
    for(size_t i=0;i<shader->machine_code_size;++i)
        value=(value^bytes[i])*UINT64_C(1099511628211);
    return value;
}
#endif
static int ps5vk_reject(const struct ps5vk_graphics_key *key,unsigned site){
    ps5vk_runtime_graphics_diag_site=site;
#if defined(PS5VK_GEOMETRY_KEY_DIAG) && PS5VK_GEOMETRY_KEY_DIAG
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_GEOMETRY_KEY site=%u topology=%u bindings=%u attributes=%u "
        "format0=%u location0=%u format1=%u location1=%u colour=%u samples=%u "
        "mask=%u blend=%u sets=%u push=%u geometry=%u tessellation=%u features=0x%x",
        site,(unsigned)key->topology,key->vertex_binding_count,key->vertex_attribute_count,
        key->vertex_attribute_count>0u?(unsigned)key->vertex_attributes[0].format:0u,
        key->vertex_attribute_count>0u?key->vertex_attributes[0].location:0u,
        key->vertex_attribute_count>1u?(unsigned)key->vertex_attributes[1].format:0u,
        key->vertex_attribute_count>1u?key->vertex_attributes[1].location:0u,
        (unsigned)key->color_format[0],(unsigned)key->samples,(unsigned)key->color_write_mask[0],
        (unsigned)key->blend_enable[0],key->descriptor_set_count,key->push_constant_size,
        (unsigned)ps5vk_graphics_has_geometry(key),
        (unsigned)ps5vk_graphics_has_tessellation(key),key->feature_mask);
#else
    (void)key;
#endif
    return 0;
}

static int blend_factor_uses_src1(VkBlendFactor factor)
{
    return factor==VK_BLEND_FACTOR_SRC1_COLOR ||
        factor==VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ||
        factor==VK_BLEND_FACTOR_SRC1_ALPHA ||
        factor==VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
}
static int blend_state_uses_src1(const struct ps5vk_graphics_key *key)
{
    for (uint32_t attachment = 0; attachment < key->color_attachment_count; ++attachment)
        if (key->blend_enable[attachment] &&
            (blend_factor_uses_src1(key->src_color_blend_factor[attachment]) ||
             blend_factor_uses_src1(key->dst_color_blend_factor[attachment]) ||
             blend_factor_uses_src1(key->src_alpha_blend_factor[attachment]) ||
             blend_factor_uses_src1(key->dst_alpha_blend_factor[attachment])))
            return 1;
    return 0;
}
/* The GFX1013 CB_BLEND0_CONTROL contract encodes the whole Vulkan 1.0 blend
 * space: native/blend_ps5.h ps5vk_blend_factor names all nineteen factors and
 * ps5vk_blend_equation names the in-register conversion for all five
 * operations. The profile serves that whole space now. The 98 applicable
 * upstream blend.dual_source leaves drew every factor and operation pair the
 * family generates and passed in one run; the native witness pinned the
 * acceptance rule for the SRC1 shape, so a SRC1 equation still needs the
 * logical-device feature and the compiler-proven secondary export before it is
 * served. */
/* Colour write mask. The upstream blend family paints each quad with a partial
 * mask (R&G, G&B, B&A) through CB_TARGET_MASK, and the profile serves those for
 * VK_FORMAT_R8G8B8A8_UNORM, whose channel order is the shader's and therefore
 * the one CB_TARGET_MASK names directly. A BGRA target keeps the all-channel
 * mask because its export applies a channel swap the mask cannot express. The
 * mask is carried in the pipeline's render-target block, never the draw
 * stream. */
static int color_write_mask_supported(const struct ps5vk_graphics_key *key)
{
    for (uint32_t attachment = 0; attachment < key->color_attachment_count; ++attachment) {
        if (key->color_format[attachment] == VK_FORMAT_R8G8B8A8_UNORM) {
            if (!key->color_write_mask[attachment] || key->color_write_mask[attachment] > 0xfu)
                return 0;
            continue;
        }
        if (key->color_write_mask[attachment] != 15) return 0;
    }
    return 1;
}
static int blend_profile_supported_one(const struct ps5vk_graphics_key *);
/* A DEPTH-ONLY pass names no colour attachment, so the pipeline records a
 * colour count of zero with an undefined colour format, writes no channel and
 * cannot blend. The four travel together: any other combination is a colour
 * target this profile does not implement, and stays refused. */
static int depth_only_target(const struct ps5vk_graphics_key *key)
{
    return !key->color_attachment_count &&
        key->color_format[0]==VK_FORMAT_UNDEFINED &&
        !key->color_write_mask[0] && !key->blend_enable[0];
}
/* The colour shapes this profile serves, one gate for both the colour and the
 * depth-only case: every named attachment must be a format this profile can
 * render into and carry a write-mask shape the render-target register can
 * express, and a subpass that names none must be the complete depth-only
 * shape. */
static int color_target_supported(const struct ps5vk_graphics_key *key)
{
    if (!key->color_attachment_count) return depth_only_target(key);
    for (uint32_t attachment = 0; attachment < key->color_attachment_count; ++attachment)
        if (key->color_format[attachment] != VK_FORMAT_B8G8R8A8_UNORM &&
            key->color_format[attachment] != VK_FORMAT_R8G8B8A8_UNORM)
            return 0;
    return color_write_mask_supported(key);
}
static int blend_profile_supported(const struct ps5vk_graphics_key *key)
{
    /* Every attachment's own equation must be one the register contract can
     * encode; a disabled attachment contributes nothing. */
    for (uint32_t attachment = 0; attachment < key->color_attachment_count; ++attachment) {
        struct ps5vk_graphics_key one = *key;
        one.blend_enable[0] = key->blend_enable[attachment];
        one.src_color_blend_factor[0] = key->src_color_blend_factor[attachment];
        one.dst_color_blend_factor[0] = key->dst_color_blend_factor[attachment];
        one.color_blend_op[0] = key->color_blend_op[attachment];
        one.src_alpha_blend_factor[0] = key->src_alpha_blend_factor[attachment];
        one.dst_alpha_blend_factor[0] = key->dst_alpha_blend_factor[attachment];
        one.alpha_blend_op[0] = key->alpha_blend_op[attachment];
        one.color_format[0] = key->color_format[attachment];
        one.color_write_mask[0] = key->color_write_mask[attachment];
        if (!blend_profile_supported_one(&one)) return 0;
    }
    return 1;
}
static int blend_profile_supported_one(const struct ps5vk_graphics_key *key)
{
    if(!key->blend_enable[0])return 1;
#if defined(PS5VK_TESS_PROBE) && PS5VK_TESS_PROBE && defined(PS5VK_TESS_VARIANT) && (PS5VK_TESS_VARIANT==20 || PS5VK_TESS_VARIANT==21)
    const VkBlendFactor source=PS5VK_TESS_VARIANT==21?VK_BLEND_FACTOR_CONSTANT_ALPHA:VK_BLEND_FACTOR_SRC_ALPHA;
    return key->blend_enable[0]==VK_TRUE &&
        key->src_color_blend_factor[0]==source &&
        key->dst_color_blend_factor[0]==VK_BLEND_FACTOR_ZERO &&
        key->color_blend_op[0]==VK_BLEND_OP_ADD &&
        key->src_alpha_blend_factor[0]==source &&
        key->dst_alpha_blend_factor[0]==VK_BLEND_FACTOR_ZERO &&
        key->alpha_blend_op[0]==VK_BLEND_OP_ADD;
#endif
    uint32_t s,d,fn,sa,da,fna;
    if(blend_state_uses_src1(key) &&
       !(key->feature_mask & PS5VK_FEATURE_DUAL_SRC_BLEND))return 0;
    return ps5vk_blend_equation(key->color_blend_op[0],key->src_color_blend_factor[0],
               key->dst_color_blend_factor[0],&s,&d,&fn) &&
           ps5vk_blend_equation(key->alpha_blend_op[0],key->src_alpha_blend_factor[0],
               key->dst_alpha_blend_factor[0],&sa,&da,&fna);
}
int ps5vk_runtime_graphics_supported(const struct ps5vk_graphics_key *key)
{
    if(!key || key->vertex.specialization_count>64 || key->fragment.specialization_count>64 ||
       key->push_constant_size>PS5VK_MAX_PUSH_CONSTANT_BYTES)return ps5vk_reject(key,1);
    /* Tessellation compiles one merged LS/HS program and a separate NGG domain.
     * This adapter checks modules, interfaces and the bounded pipeline shape;
     * it is not a feature-promotion gate. Native queue ownership and feature
     * negotiation are checked independently. */
    if(ps5vk_graphics_has_tessellation(key)) {
        if(ps5vk_graphics_has_geometry(key) &&
           (key->geometry.specialization_count>64 || !module_supported(&key->geometry,3)))
            return ps5vk_reject(key,3);
        if(key->topology!=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)return ps5vk_reject(key,2);
        if(!ps5vk_graphics_tessellation_key_valid(key))return ps5vk_reject(key,3);
        if(!module_supported(&key->tess_control,1) ||
           !module_supported(&key->tess_eval,2))return ps5vk_reject(key,4);
        if(!ps5vk_spirv_graphics_interface(key))return ps5vk_reject(key,5);
        uint32_t patch_type=0;
        if(!ps5vk_tess_patch_primitive_type(&patch_type))return ps5vk_reject(key,6);
        if(key->samples!=VK_SAMPLE_COUNT_1_BIT ||
           !color_target_supported(key) ||
           !blend_profile_supported(key))return ps5vk_reject(key,8);
        if(!descriptor_profile_supported(key))return ps5vk_reject(key,9);
        return 1;
    }
    /* A geometry stage is compiled through the merged entry point, so its own
     * module passes the same structural screening as the other two. The merged
     * program IS accepted now: the ES->GS input handoff was the one thing that
     * made it unsafe, and it is fixed and witnessed on hardware - the hardware
     * scales the per-vertex offsets it hands the geometry half by
     * VGT_ESGS_RING_ITEMSIZE, next-gen geometry keeps that at one so the offsets
     * stay item indices, and the driver now programs it that way instead of with
     * the compiler's legacy item size (which scaled them twice: the geometry half
     * read item 5k where it must read item k, so only the first vertex of each
     * primitive ever came back right). The whole witness table, including a
     * per-item readback of what the stage read, verifies on the console with this
     * path. What remains before the FEATURE can be advertised is the applicable
     * conformance selection, not this adapter. */
    if(ps5vk_graphics_has_geometry(key)) {
        if(!module_supported(&key->geometry,3))return ps5vk_reject(key,6);
        /* A geometry stage that writes gl_ViewportIndex selects among the
         * viewport banks the pipeline programs, and this profile programs one
         * bank unless the logical device enabled multiViewport. Accepting the
         * declaration while the feature is off would run a draw whose routing
         * silently collapses to viewport zero, so it is refused here on the
         * enabled mask - the same mask, and the same place, every cached pair is
         * re-checked against on acquisition. */
        if(ps5vk_spirv_stage_viewport_index(&key->geometry) &&
           !(key->feature_mask & PS5VK_FEATURE_MULTI_VIEWPORT))return ps5vk_reject(key,14);
    }
    for(unsigned i=0;i<PS5VK_MAX_PUSH_CONSTANT_DWORDS;++i)
        if(key->push_constant_stages[i]&~(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT))return ps5vk_reject(key,7);
    if(key->vertex_binding_count>16 || key->vertex_attribute_count>PSBC_MAX_VERTEX_ATTRIBUTES ||
       (key->vertex_binding_count && !key->vertex_bindings) ||
       (key->vertex_attribute_count && !key->vertex_attributes))return ps5vk_reject(key,8);
    for(uint32_t i=0;i<key->vertex_binding_count;++i) {
        const VkVertexInputBindingDescription *b=&key->vertex_bindings[i];
        if(b->binding>=16 || b->inputRate!=VK_VERTEX_INPUT_RATE_VERTEX ||
           !b->stride || b->stride>0x3fff)return ps5vk_reject(key,9);
        for(uint32_t j=0;j<i;++j)if(key->vertex_bindings[j].binding==b->binding)return ps5vk_reject(key,10);
    }
    for(uint32_t i=0;i<key->vertex_attribute_count;++i) {
        const VkVertexInputAttributeDescription *a=&key->vertex_attributes[i];
        if(a->binding>=16 || a->location>=PSBC_MAX_VERTEX_ATTRIBUTES)return ps5vk_reject(key,11);
        unsigned found=0;
        for(uint32_t j=0;j<key->vertex_binding_count;++j)found|=key->vertex_bindings[j].binding==a->binding;
        if(!found)return ps5vk_reject(key,12);
        for(uint32_t j=0;j<i;++j)if(key->vertex_attributes[j].location==a->location)return ps5vk_reject(key,13);
    }
    uint32_t primitive_type=0;
    /* A fragment stage that READS gl_ClipDistance/gl_CullDistance is not
     * screened out here: whether this profile can deliver the read is a fact
     * about the compiled metadata - does the pixel input list name the packed
     * distance registers, and does the pre-raster stage describe the same ones -
     * and that is decided where the metadata exists (see the delivery check in
     * ps5vk_runtime_graphics_compile). The declaration itself is already
     * bounded by the interface chain above. */
    if(!module_supported(&key->vertex,0) || !module_supported(&key->fragment,4))
        return ps5vk_reject(key,20);
    if(ps5vk_agc_primitive_type(key->topology,&primitive_type))return ps5vk_reject(key,21);
        /* A point or line input primitive is accepted only when there is a
         * geometry stage to feed it: that is the shape the native witness
         * measures, and a plain point/line pipeline stays fail-closed. */
    if(!ps5vk_graphics_has_geometry(key) && ps5vk_agc_primitive_needs_geometry(primitive_type))
        return ps5vk_reject(key,22);
    if(!color_target_supported(key) ||
       key->samples!=VK_SAMPLE_COUNT_1_BIT ||
       !blend_profile_supported(key))return ps5vk_reject(key,23);
    /* Binding counts/pointers were checked above. Keep every remaining
     * refusal observable, including the non-tessellated CTS reference path. */
    if(!descriptor_profile_supported(key))return ps5vk_reject(key,24);
    if(!ps5vk_spirv_graphics_interface(key))return ps5vk_reject(key,25);
    return 1;
}

static PsbcVertexFormat vertex_format(VkFormat format)
{
    switch(format) {
    case VK_FORMAT_R32_SFLOAT: return PSBC_VERTEX_FORMAT_R32_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT: return PSBC_VERTEX_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R32G32B32_SFLOAT: return PSBC_VERTEX_FORMAT_R32G32B32_FLOAT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return PSBC_VERTEX_FORMAT_R32G32B32A32_FLOAT;
    case VK_FORMAT_R32_SINT: return PSBC_VERTEX_FORMAT_R32_SINT;
    case VK_FORMAT_R32G32_SINT: return PSBC_VERTEX_FORMAT_R32G32_SINT;
    case VK_FORMAT_R32G32B32_SINT: return PSBC_VERTEX_FORMAT_R32G32B32_SINT;
    case VK_FORMAT_R32G32B32A32_SINT: return PSBC_VERTEX_FORMAT_R32G32B32A32_SINT;
    case VK_FORMAT_R32_UINT: return PSBC_VERTEX_FORMAT_R32_UINT;
    case VK_FORMAT_R32G32_UINT: return PSBC_VERTEX_FORMAT_R32G32_UINT;
    case VK_FORMAT_R32G32B32_UINT: return PSBC_VERTEX_FORMAT_R32G32B32_UINT;
    case VK_FORMAT_R32G32B32A32_UINT: return PSBC_VERTEX_FORMAT_R32G32B32A32_UINT;
    case VK_FORMAT_R8_UNORM: return PSBC_VERTEX_FORMAT_R8_UNORM;
    case VK_FORMAT_R8_SNORM: return PSBC_VERTEX_FORMAT_R8_SNORM;
    case VK_FORMAT_R8_UINT: return PSBC_VERTEX_FORMAT_R8_UINT;
    case VK_FORMAT_R8_SINT: return PSBC_VERTEX_FORMAT_R8_SINT;
    case VK_FORMAT_R8G8_UNORM: return PSBC_VERTEX_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8_SNORM: return PSBC_VERTEX_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R8G8_UINT: return PSBC_VERTEX_FORMAT_R8G8_UINT;
    case VK_FORMAT_R8G8_SINT: return PSBC_VERTEX_FORMAT_R8G8_SINT;
    case VK_FORMAT_R8G8B8A8_UNORM: return PSBC_VERTEX_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return PSBC_VERTEX_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return PSBC_VERTEX_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_A8B8G8R8_SNORM_PACK32: return PSBC_VERTEX_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_A8B8G8R8_UINT_PACK32: return PSBC_VERTEX_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_A8B8G8R8_SINT_PACK32: return PSBC_VERTEX_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return PSBC_VERTEX_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16_UNORM: return PSBC_VERTEX_FORMAT_R16_UNORM;
    case VK_FORMAT_R16_SNORM: return PSBC_VERTEX_FORMAT_R16_SNORM;
    case VK_FORMAT_R16_UINT: return PSBC_VERTEX_FORMAT_R16_UINT;
    case VK_FORMAT_R16_SINT: return PSBC_VERTEX_FORMAT_R16_SINT;
    case VK_FORMAT_R16_SFLOAT: return PSBC_VERTEX_FORMAT_R16_FLOAT;
    case VK_FORMAT_R16G16_UNORM: return PSBC_VERTEX_FORMAT_R16G16_UNORM;
    case VK_FORMAT_R16G16_SNORM: return PSBC_VERTEX_FORMAT_R16G16_SNORM;
    case VK_FORMAT_R16G16_UINT: return PSBC_VERTEX_FORMAT_R16G16_UINT;
    case VK_FORMAT_R16G16_SINT: return PSBC_VERTEX_FORMAT_R16G16_SINT;
    case VK_FORMAT_R16G16_SFLOAT: return PSBC_VERTEX_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R16G16B16A16_UNORM: return PSBC_VERTEX_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16A16_SNORM: return PSBC_VERTEX_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16A16_UINT: return PSBC_VERTEX_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16A16_SINT: return PSBC_VERTEX_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return PSBC_VERTEX_FORMAT_R16G16B16A16_FLOAT;
    default: return PSBC_VERTEX_FORMAT_NONE;
    }
}

VkResult ps5vk_runtime_graphics_descriptor_options(const struct ps5vk_graphics_key *key,
    VkShaderStageFlags stages,PsbcCompileOptions *options)
{
    /* `stages` is the set of stages whose bindings the table must carry: a
     * single stage for a standalone compilation, every stage of the merged
     * pre-raster program when the compiler links a vertex+geometry pair, and
     * the vertex+control or evaluation stage of a tessellation pipeline. */
    if(!key || !options || !(stages&(VK_SHADER_STAGE_VERTEX_BIT|
            VK_SHADER_STAGE_GEOMETRY_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|
            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT|
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)))
        return VK_ERROR_UNKNOWN;
    struct ps5vk_descriptor_table_layout tables;
    VkResult rc=ps5vk_descriptor_table_layout_build(key->descriptor_set_count,
        key->descriptor_sets,&tables);
    if(rc!=VK_SUCCESS)return rc;
    PsbcDescriptorBinding bindings[PSBC_MAX_DESCRIPTOR_BINDINGS]={0};
    uint32_t count=0;
    for(uint32_t s=0;s<key->descriptor_set_count;++s)
        for(uint32_t b=0;b<PS5VK_MAX_BINDINGS;++b) {
            const struct ps5vk_binding *source=&key->descriptor_sets[s].binding[b];
            /* An input attachment is fragment-visible resource-only image data.
             * A declaration that exposes it to any other stage is not a
             * declaration this profile can honour, so it fails closed here
             * rather than being projected into a table that no longer says what
             * the caller wrote. */
            if(source->count &&
               key->descriptor_sets[s].type[b]==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT &&
               source->stages!=VK_SHADER_STAGE_FRAGMENT_BIT)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            if(!source->count || !(source->stages&stages))continue;
            if(count==PSBC_MAX_DESCRIPTOR_BINDINGS)return VK_ERROR_FEATURE_NOT_PRESENT;
            PsbcDescriptorType type;
            switch(key->descriptor_sets[s].type[b]) {
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: type=PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER;break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: type=PSBC_DESCRIPTOR_UNIFORM_BUFFER;break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: type=PSBC_DESCRIPTOR_STORAGE_BUFFER;break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: type=PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER;break;
            /* Fragment-only resource-only image data: the canonical table gives
             * this role its own 32-byte record, so the PSBC type is the
             * resource-only one and never the combined T#/S# pair. The stage
             * projection above already keeps it out of every other stage. */
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: type=PSBC_DESCRIPTOR_INPUT_ATTACHMENT;break;
            default:return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            bindings[count++]=(PsbcDescriptorBinding){.set=s,.binding=b,.type=type,
                .array_size=source->count,.offset=tables.binding[s][b].byte_offset,
                .stride=tables.binding[s][b].byte_stride};
        }
    memcpy(options->descriptor_bindings,bindings,sizeof(bindings));
    options->descriptor_binding_count=count;
    /* The layout is a declaration, not evidence of use: the compiler reports
     * the sets and bindings the optimized NIR really dereferences, so a layout
     * set no shader touches cannot acquire a native descriptor dependency. */
    options->static_descriptor_use=true;
    return VK_SUCCESS;
}

static int linked_parameters(PsbcLinkedStageParameters *out,
                            const struct ps5vk_graphics_module_key *module)
{
    if(module->specialization_count>PSBC_MAX_SPECIALIZATION_CONSTANTS)return 0;
    *out=(PsbcLinkedStageParameters){.enabled=true,.entrypoint=module->entry,
        .specialization_constant_count=module->specialization_count};
    for(uint32_t i=0;i<module->specialization_count;++i) {
        if(!module->specializations[i].size || module->specializations[i].size>8)return 0;
        out->specialization_constants[i].constant_id=module->specializations[i].constant_id;
        out->specialization_constants[i].size=module->specializations[i].size;
        memcpy(out->specialization_constants[i].data,module->specializations[i].data,8);
        for(uint32_t j=0;j<i;++j)
            if(module->specializations[j].constant_id==module->specializations[i].constant_id)return 0;
    }
    return 1;
}

static int apply_parameters(PsbcCompileOptions *options,
                            const struct ps5vk_graphics_module_key *module,
                            const struct ps5vk_graphics_key *key,VkShaderStageFlags stages)
{
    if(module->specialization_count>PSBC_MAX_SPECIALIZATION_CONSTANTS)return 0;
    options->specialization_constant_count=module->specialization_count;
    for(uint32_t i=0;i<module->specialization_count;++i) {
        if(!module->specializations[i].size || module->specializations[i].size>8)return 0;
        options->specialization_constants[i].constant_id=module->specializations[i].constant_id;
        options->specialization_constants[i].size=module->specializations[i].size;
        memcpy(options->specialization_constants[i].data,module->specializations[i].data,8);
    }
    for(uint32_t i=0;i<module->specialization_count;++i)
        for(uint32_t j=0;j<i;++j)
            if(module->specializations[i].constant_id==module->specializations[j].constant_id)return 0;
    options->force_indirect_push_constants=false;
    for(unsigned i=0;i<PS5VK_MAX_PUSH_CONSTANT_DWORDS;++i)
        if(key->push_constant_stages[i]&stages)options->force_indirect_push_constants=true;
    options->vertex_attribute_count=0;
    if(ps5vk_runtime_graphics_descriptor_options(key,stages,options)!=VK_SUCCESS)return 0;
    if(stages&VK_SHADER_STAGE_VERTEX_BIT) {
        for(uint32_t i=0;i<key->vertex_attribute_count;++i) {
            const VkVertexInputAttributeDescription *source=&key->vertex_attributes[i];
            const VkVertexInputBindingDescription *binding=NULL;
            for(uint32_t j=0;j<key->vertex_binding_count;++j)
                if(key->vertex_bindings[j].binding==source->binding)binding=&key->vertex_bindings[j];
            PsbcVertexFormat format=vertex_format(source->format);
            if(!binding || !format || binding->inputRate!=VK_VERTEX_INPUT_RATE_VERTEX ||
               !binding->stride || binding->stride>0x3fff)
                return 0;
            options->vertex_attributes[options->vertex_attribute_count++]=(PsbcVertexAttribute){
                .location=(uint8_t)source->location,.binding=(uint8_t)source->binding,
                .format=format,.offset=source->offset,.stride=binding->stride,
                /* Vulkan vertex bindings and offsets are byte-granular. */
                .alignment=1,.instance_divisor=0};
        }
    }
    return 1;
}

static int push_metadata_supported(const PsbcShaderMetadata *metadata,
                                   const struct ps5vk_graphics_key *key,
                                   VkShaderStageFlagBits stage)
{
    if(!metadata->push_use_valid)return 0;
    const uint64_t reads=metadata->stage_push_dwords|metadata->previous_stage_push_dwords;
    if(!metadata->push_constants_valid)return !metadata->push_constant_size && !reads;
    if(!metadata->push_constant_size || metadata->push_constant_size>key->push_constant_size)return 0;
    if(metadata->previous_stage_push_dwords && stage!=VK_SHADER_STAGE_GEOMETRY_BIT)return 0;
    return reads && ps5vk_runtime_push_visibility(metadata->stage_push_dwords,
        key->push_constant_stages,(metadata->push_constant_size+3u)/4u,stage) &&
        ps5vk_runtime_push_visibility(metadata->previous_stage_push_dwords,
        key->push_constant_stages,(metadata->push_constant_size+3u)/4u,
        metadata->merged_es_source_stage==PSBC_STAGE_TESS_EVAL?
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:VK_SHADER_STAGE_VERTEX_BIT);
}

VkResult ps5vk_runtime_graphics_compile(void *context,const struct ps5vk_graphics_key *key,const void **out)
{
    (void)context;
    if(!out)return VK_ERROR_UNKNOWN;
    *out=NULL;
    if(!ps5vk_runtime_graphics_supported(key))return VK_ERROR_FEATURE_NOT_PRESENT;
    struct ps5vk_runtime_graphics_program *p=calloc(1,sizeof(*p));
    if(!p)return VK_ERROR_OUT_OF_HOST_MEMORY;
    PsbcResult result=PSBC_RESULT_INTERNAL_ERROR;
    VkResult failure=VK_ERROR_FEATURE_NOT_PRESENT;
    PsbcCompileOptions options={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_FRAGMENT,
        .entrypoint=key->fragment.entry,.optimise=true,.address32_hi=2,
        .primitive_type=0,.rasterization_samples=1};
    /* Mesa ac_choose_spi_color_formats: RGBA8 UNORM blending uses FP16_ABGR,
     * paired with matching SX conversion at draw time; unblended keeps 32_ABGR.
     * The option is one nibble per colour attachment, so it is derived from the
     * attachment's own blend state rather than from the pipeline as a whole:
     * the pinned compiler drops a second export whose nibble is not declared
     * (measured), which is what makes the derivation the honest form even
     * while this profile serves one target. */
    {
        unsigned char blending[PS5VK_MAX_COLOR_ATTACHMENTS]={0};
        for(uint32_t attachment=0;attachment<key->color_attachment_count;++attachment)
            blending[attachment]=key->blend_enable[attachment]?1u:0u;
        options.spi_shader_col_format=
            ps5vk_color_export_format_option(blending,key->color_attachment_count);
        options.color_is_int8=0;
    }
    /* A patch-list draw feeds the patch assembler the pinned gfx103 register
     * data names DI_PT_PATCH; every other topology resolves the value the AGC
     * linker programs. The DI patch type is the DRAW's identity, not a compile
     * option: the tessellator, not the assembler, generates the rasterized
     * primitive, so the shader compiles keep the compiler's default primitive
     * state and the flat/provoking semantics of the tessellated primitive are
     * the witness slice's measurement, not a guess here. */
    const int has_tessellation=ps5vk_graphics_has_tessellation(key);
    if(has_tessellation) {
        if(!ps5vk_tess_patch_primitive_type(&p->primitive_type))goto failed;
    } else if(ps5vk_agc_primitive_type(key->topology,&options.primitive_type))
        goto failed;
    if(!apply_parameters(&options,&key->fragment,key,VK_SHADER_STAGE_FRAGMENT_BIT))goto failed;
    const struct ps5vk_graphics_module_key *distance_producer=
        ps5vk_graphics_has_geometry(key)?&key->geometry:
        ps5vk_graphics_has_tessellation(key)?&key->tess_eval:&key->vertex;
    unsigned producer_clip_count=0,producer_cull_count=0;
    if(!ps5vk_spirv_stage_distance_declarations(distance_producer,
           &producer_clip_count,&producer_cull_count))goto failed;
    options.fragment_distance_layout_valid=true;
    options.fragment_clip_distance_count=producer_clip_count;
    result=psbc_compile_shader(key->fragment.words,key->fragment.word_count*4u,&options,&p->fragment);
    if(result!=PSBC_RESULT_OK)goto failed;
    {
        const int fragment_export=ps5vk_runtime_fragment_export(&p->fragment.metadata);
        if(fragment_export<0)goto failed;
        unsigned primary_mask=0;
        int secondary=0;
        if(!ps5vk_spirv_fragment_outputs(&key->fragment,&primary_mask,&secondary))
            goto failed;
        p->fragment_shape=PS5VK_RUNTIME_FRAGMENT_SHAPE_SINGLE;
        p->dual_source_export=0;
        if(fragment_export==PS5VK_RUNTIME_FRAGMENT_EXPORT_NONE) {
            /* A stage that reaches no colour store exports nothing. Two legal
             * shapes: the DEPTH-ONLY pass, whose stage declares no output at
             * all, and the ordinary colour pass whose only reachable side
             * effect is an SSBO store - the pinned frag_side_effects kill
             * leaves, where glslang keeps the Output in the entry point but
             * removes the unreachable store. The interface policy already ties
             * the declaration to the key, so the export has to agree with it
             * and carry no secondary. */
            const unsigned expected=key->color_attachment_count?1u:0u;
            if(secondary || primary_mask!=expected)goto failed;
        } else if(fragment_export==PS5VK_RUNTIME_FRAGMENT_EXPORT_DUAL) {
            /* The pinned compiler publishes 0x44/0xff for both shapes. The
             * interface decides which one this is; a package whose registers
             * and interface disagree is torn and fails. */
            if(secondary && primary_mask==1u) {
                p->fragment_shape=PS5VK_RUNTIME_FRAGMENT_SHAPE_DUAL;
                p->dual_source_export=1u;
            } else if(primary_mask==3u && !secondary) {
                /* Two MRTs, proven against the pinned compiler: the export is
                 * real, but the profile does not serve a second target yet -
                 * the advertised limit is one colour attachment and no draw
                 * has written two - so the pipeline stays refused. Refusing
                 * here is what keeps the second export from being dropped
                 * silently; the slice that witnesses a two-target draw lifts
                 * this together with the limit. */
                p->fragment_shape=PS5VK_RUNTIME_FRAGMENT_SHAPE_TWO_MRT;
                goto failed;
            } else goto failed;
        } else if(secondary || primary_mask!=1u) {
            /* A secondary or a second location without its register pair is a
             * torn package. */
            goto failed;
        }
        /* A SRC1 equation consumes the secondary export.  Device enablement
         * alone cannot manufacture it: ordinary and torn fragment packages
         * must fail before a native pair can be allocated. */
        if(blend_state_uses_src1(key) && !p->dual_source_export)goto failed;
    }
    /* Compile FS first so its actual metadata can prove PrimitiveID is unused. */
    if(p->fragment.metadata.input_semantic_count>PSBC_MAX_SEMANTICS)goto failed;
    for(unsigned i=0;i<p->fragment.metadata.input_semantic_count;++i)
        if((p->fragment.metadata.input_semantics[i]&255u)==PSBC_SEMANTIC_PRIMITIVE_ID)goto failed;
    if(has_tessellation) {
        /* The pre-raster state is two programs: the hull (the compiler links
         * the vertex half as the LS program behind the control half, whose
         * metadata this output carries) and the domain (the evaluation half's
         * loadable NGG package). Source-stage specialization namespaces remain
         * independent even when the hardware merges their programs. */
        PsbcCompileOptions hull_options={.target=PSBC_TARGET_PS5,
            .stage=PSBC_STAGE_TESS_CTRL,.entrypoint=key->tess_control.entry,
            .optimise=true,.address32_hi=2,.rasterization_samples=1,
            /* The input patch size is what makes the hull compile derive its
             * workgroup layout; without it the metadata publishes no tess
             * workgroup state and the native loader refuses the hull. */
            .patch_control_points=key->patch_control_points};
        if(!linked_parameters(&hull_options.previous_parameters,&key->vertex) ||
           !linked_parameters(&hull_options.next_link_parameters,&key->tess_eval))goto failed;
        if(!apply_parameters(&hull_options,&key->tess_control,key,
                VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT))goto failed;
        /* The evaluation half goes in LINK-ONLY, so the control half learns
         * the tessellator configuration it cannot declare for itself: the
         * domain, spacing, winding and point mode live in the .tese, and the
         * tessellation-factor layout the hull stores depends on the domain. */
        result=psbc_compile_tess_pipeline(key->vertex.words,key->vertex.word_count*4u,
            key->tess_control.words,key->tess_control.word_count*4u,
            key->tess_eval.words,key->tess_eval.word_count*4u,
            &hull_options,&p->hull);
        if(result!=PSBC_RESULT_OK)goto failed;
        /* One pointer serves both halves, but visibility belongs to each
         * source stage. Metadata18 preserves their pre-link read masks;
         * combined prefix size alone cannot validate disjoint ranges. */
        const PsbcShaderMetadata *hm=&p->hull.metadata;
        const uint64_t push_reads=hm->hull_vertex_push_dwords|hm->hull_control_push_dwords;
        if(!hm->hull_push_use_valid || hm->push_constant_size>key->push_constant_size ||
           (!!push_reads != !!hm->push_constants_valid) ||
           !ps5vk_runtime_push_visibility(hm->hull_vertex_push_dwords,key->push_constant_stages,
                (hm->push_constant_size+3u)/4u,VK_SHADER_STAGE_VERTEX_BIT) ||
           !ps5vk_runtime_push_visibility(hm->hull_control_push_dwords,key->push_constant_stages,
                (hm->push_constant_size+3u)/4u,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT))goto failed;
        if(ps5vk_runtime_hull_abi_build(&p->hull.metadata,&p->hull_arguments))goto failed;
        PsbcCompileOptions domain_options={.target=PSBC_TARGET_PS5,
            .stage=PSBC_STAGE_TESS_EVAL,.entrypoint=key->tess_eval.entry,
            .optimise=true,.ngg=true,.address32_hi=2,.rasterization_samples=1,
            /* The domain is LINKED against the control half, which is what
             * keeps num_tess_patches, the attribute stride and
             * tes_reads_tess_factors compile-time constants. Compiled alone
             * it reads all three at runtime from the tcs_offchip_layout user
             * SGPR, an ABI nothing here supplies. The patch size is what the
             * control half's patch-count derivation needs. */
            .patch_control_points=key->patch_control_points,
            /* The device facts the GE's parameter-cache allocation needs.
             *
             * radv programs GE_PC_ALLOC for every NGG pipeline, and psbc will
             * compute it through the same ac_compute_late_alloc() radv uses -
             * but only when the caller supplies these. Nothing in this driver
             * ever supplied them, for any pipeline, so the register has never
             * been programmed and every NGG draw has run on whatever the
             * platform left there. That is tolerable for the vertex-fed
             * pipelines that pass; it is the last register in radv's tracked
             * per-draw set that a tessellation pipeline also leaves alone, and
             * a domain shader's parameter-cache allocation happens after
             * tessellation rather than as vertices arrive.
             *
             * The values are the pinned tables, not estimates: ac_gpu_info.c
             * names CHIP_GFX1013 explicitly with pc_lines = 1024, and the
             * eighteen good compute units per shader array are the same
             * measured topology this driver's tessellation ring sizing already
             * uses. Culling is off and this domain needs no scratch. */
            .ngg_device_facts=true,
            .ngg_pc_lines=1024u,
            .ngg_min_good_cu_per_sa=18u,
            .ngg_culling=false,
            .ngg_uses_scratch=false,
            /* DIAGNOSTIC BISECT, default off: the domain compiled with NGG
             * passthrough forced off. Every passing NGG draw on this device
             * is vertex-fed, so passthrough has never been exercised WITH a
             * domain shader here, and the measurements now say the hull runs,
             * its factors are correct, and the evaluation half never
             * launches. */
#if defined(PS5VK_TESS_NO_PASSTHRU) && PS5VK_TESS_NO_PASSTHRU
            .ngg_no_passthrough=true,
#endif
            };
        const int merged_geometry=ps5vk_graphics_has_geometry(key);
        if(merged_geometry) {
            domain_options.stage=PSBC_STAGE_GEOMETRY;
            domain_options.entrypoint=key->geometry.entry;
            if(!apply_parameters(&domain_options,&key->geometry,key,
                    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT|VK_SHADER_STAGE_GEOMETRY_BIT) ||
               !linked_parameters(&domain_options.previous_parameters,&key->tess_eval) ||
               !linked_parameters(&domain_options.next_link_parameters,&key->tess_control))goto failed;
            result=psbc_compile_tess_geometry_pipeline(
                key->tess_control.words,key->tess_control.word_count*4u,
                key->tess_eval.words,key->tess_eval.word_count*4u,
                key->geometry.words,key->geometry.word_count*4u,&domain_options,&p->domain);
        } else {
            if(!apply_parameters(&domain_options,&key->tess_eval,key,
                    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT))goto failed;
            if(!linked_parameters(&domain_options.previous_parameters,&key->tess_control))goto failed;
            result=psbc_compile_domain_pipeline(
                key->tess_control.words,key->tess_control.word_count*4u,
                key->tess_eval.words,key->tess_eval.word_count*4u,
                &domain_options,&p->domain);
        }
        if(result!=PSBC_RESULT_OK)goto failed;
        if(!push_metadata_supported(&p->domain.metadata,key,
                merged_geometry?VK_SHADER_STAGE_GEOMETRY_BIT:VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) ||
           !push_metadata_supported(&p->fragment.metadata,key,
                VK_SHADER_STAGE_FRAGMENT_BIT))goto failed;
        /* The compiled halves are the usage evidence: a tessellation pipeline
         * really runs the tessellator, so both halves' source stages require
         * the feature the application must have enabled, and the pixel end of
         * the distance interface is checked against the pre-raster stage the
         * fragment stage actually reads from - the evaluation half. */
        if(!ps5vk_runtime_graphics_feature_use_ok(&p->hull.metadata,
                &p->fragment.metadata,key->feature_mask) ||
           !ps5vk_runtime_graphics_feature_use_ok(&p->domain.metadata,
                &p->fragment.metadata,key->feature_mask))goto failed;
        unsigned declared_clip=0,declared_cull=0;
        if(!ps5vk_spirv_stage_distance_reads(&key->fragment,&declared_clip,&declared_cull))goto failed;
        if((declared_clip||declared_cull) &&
           !ps5vk_runtime_graphics_distance_reads_described(&p->domain.metadata,
               &p->fragment.metadata,declared_clip,declared_cull))goto failed;
        /* The domain half is packaged through the same runtime header the
         * vertex programs use - it is an NGG pre-raster program - and the draw
         * ABI is built from its metadata and the fragment's. The hull half is
         * NOT packaged here: its runtime header would need the hull launch
         * state the native create path owns (the load gate keeps refusing the
         * hull, which is that contract, not a bug). */
        struct ps5vk_runtime_shader header;
#if defined(PS5VK_TESS_RING_QUERY) && PS5VK_TESS_RING_QUERY == 4
        /* Diagnostic discriminator, not a cryptographic artifact identity. */
        ps5log_printf(PS5LOG_MARK,
            "PS5VK_TESS_CODE_FNV64 hull=%016llx domain=%016llx fragment=%016llx hull_bytes=%zu domain_bytes=%zu fragment_bytes=%zu",
            (unsigned long long)tess_code_fingerprint(&p->hull),
            (unsigned long long)tess_code_fingerprint(&p->domain),
            (unsigned long long)tess_code_fingerprint(&p->fragment),
            p->hull.machine_code_size,p->domain.machine_code_size,p->fragment.machine_code_size);
#endif
        if(ps5vk_runtime_shader_build(&header,&p->domain) ||
           ps5vk_runtime_shader_build(&header,&p->fragment) ||
           ps5vk_runtime_draw_abi_build(&p->domain.metadata,&p->fragment.metadata,
               &p->arguments))goto failed;
#if defined(PS5VK_TESS_LEGACY_DOMAIN) && PS5VK_TESS_LEGACY_DOMAIN
        /* DIAGNOSTIC: the same evaluation half as a legacy hardware VS. Same
         * link against the control half, same device facts (radv programs
         * GE_PC_ALLOC for a legacy VS too), NGG off. Its ABI is derived from
         * the NGG domain's validated one; see derive_legacy_abi(). */
        {
            PsbcCompileOptions legacy_options=domain_options;
            legacy_options.ngg=false;
            legacy_options.ngg_no_passthrough=false;
            result=psbc_compile_domain_pipeline(
                key->tess_control.words,key->tess_control.word_count*4u,
                key->tess_eval.words,key->tess_eval.word_count*4u,
                &legacy_options,&p->domain_legacy);
            if(result!=PSBC_RESULT_OK)goto failed;
            if(derive_legacy_abi(&p->arguments,&p->domain_legacy.metadata,
                   &p->arguments_legacy))goto failed;
            p->domain_legacy_valid=1;
        }
#endif
        /* The draw's DI patch type was recorded when it was resolved. The
         * patch control points ride along for the launch state. */
        p->patch_control_points=key->patch_control_points;
        p->tess_output_points=ps5vk_spirv_tess_pair_output_points(key);
        if(!p->tess_output_points)goto failed;
        *out=p;
        return VK_SUCCESS;
    }
    options.ngg=true;
    options.entrypoint=key->vertex.entry;options.omit_implicit_primitive_id=true;
    if(!apply_parameters(&options,&key->vertex,key,VK_SHADER_STAGE_VERTEX_BIT))goto failed;
    if(ps5vk_graphics_has_geometry(key)) {
        /* A geometry pipeline's pre-raster stage is the merged vertex+geometry
         * program: the compiler links the pair, so the compiled metadata
         * describes the last programmable stage the hardware runs before
         * rasterization. The geometry stage's own entry point and descriptors
         * come from its module key. */
        options.stage=PSBC_STAGE_GEOMETRY;
        options.entrypoint=key->geometry.entry;
        /* One merged shader carries one specialization map. The module that has
         * constants supplies it; a pipeline that specializes both halves is
         * refused instead of silently keeping one of the two maps. */
        if(key->geometry.specialization_count && key->vertex.specialization_count)goto failed;
        const struct ps5vk_graphics_module_key *specialized=
            key->geometry.specialization_count?&key->geometry:&key->vertex;
        if(!apply_parameters(&options,specialized,key,
                VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_GEOMETRY_BIT))goto failed;
        result=psbc_compile_geometry_pipeline(key->vertex.words,key->vertex.word_count*4u,
            key->geometry.words,key->geometry.word_count*4u,&options,&p->vertex);
    } else {
        options.stage=PSBC_STAGE_VERTEX;
        result=psbc_compile_shader(key->vertex.words,key->vertex.word_count*4u,&options,&p->vertex);
    }
    if(result!=PSBC_RESULT_OK)goto failed;
    if(!push_metadata_supported(&p->vertex.metadata,key,
            ps5vk_graphics_has_geometry(key)?VK_SHADER_STAGE_GEOMETRY_BIT:VK_SHADER_STAGE_VERTEX_BIT) ||
       !push_metadata_supported(&p->fragment.metadata,key,VK_SHADER_STAGE_FRAGMENT_BIT))goto failed;
    /* The compiled stages are the usage evidence: refuse a pair that really
     * consumes a capability the application never enabled. */
    if(!ps5vk_runtime_graphics_feature_use_ok(&p->vertex.metadata,&p->fragment.metadata,
        key->feature_mask))goto failed;
    /* The pixel end of the clip/cull interface is delivered only when the
     * compiled metadata describes it end to end: the pre-raster stage names each
     * packed distance register it exports (parameter index included) and the
     * pixel stage names the same registers as inputs, which is what the AGC
     * linker turns into the pixel-input control. A module that reads a distance
     * while either list is incomplete is refused here - the attributes would be
     * interpolated from registers nothing names. The shipping profile also
     * requires native evidence for the read before it runs one, exactly like the
     * geometry path: the witness measures it under the diagnostic profile. */
    {
        unsigned declared_clip=0,declared_cull=0;
        if(!ps5vk_spirv_stage_distance_reads(&key->fragment,&declared_clip,&declared_cull))goto failed;
        if((declared_clip||declared_cull) &&
           !ps5vk_runtime_graphics_distance_reads_described(&p->vertex.metadata,
               &p->fragment.metadata,declared_clip,declared_cull))goto failed;
        /* A described read is delivered in every profile, including the shipping
         * one: the rasterizer handing the interpolated distance to the pixel
         * stage is measured, not assumed. Two runs of the eleven-case clip/cull
         * witness, on two different builds, verify case 10 (the fragment stage
         * reading gl_ClipDistance[0]) with expected=4096 covered=4096
         * foreign=0 wrong_color=0 and digest f50dd9368fee6cc9, and the
         * acceptance run is strict_verified with the pixel-read digest differing
         * from the control's. What this does NOT do is advertise the feature:
         * the device reports it false until its own obligations and the
         * applicable CTS acceptance are complete, and a pair that uses a
         * distance still needs the application to enable it (feature_use_ok). */
    }
    struct ps5vk_runtime_shader header;
    if(ps5vk_runtime_shader_build(&header,&p->vertex) ||
       ps5vk_runtime_shader_build(&header,&p->fragment) ||
       ps5vk_runtime_draw_abi_build(&p->vertex.metadata,&p->fragment.metadata,&p->arguments))goto failed;
#if defined(PS5VK_TESS_LEGACY_VS_CONTROL) && PS5VK_TESS_LEGACY_VS_CONTROL
    /* DIAGNOSTIC, the positive control for the legacy-domain experiment: a
     * plain vertex pipeline compiled a second time as a LEGACY hardware VS and
     * launched that way. No legacy (non-NGG) VS has ever run on this device -
     * every working pipeline here is NGG - so until a legacy triangle draws, a
     * legacy tessellation stall says nothing about tessellation. Geometry
     * pipelines stay NGG: their pre-raster program is the merged pair. */
    if(!ps5vk_graphics_has_geometry(key)) {
        PsbcCompileOptions legacy_options=options;
        legacy_options.ngg=false;
        legacy_options.ngg_no_passthrough=false;
        result=psbc_compile_shader(key->vertex.words,key->vertex.word_count*4u,
            &legacy_options,&p->domain_legacy);
        if(result!=PSBC_RESULT_OK)goto failed;
        if(derive_legacy_abi(&p->arguments,&p->domain_legacy.metadata,
               &p->arguments_legacy))goto failed;
        p->domain_legacy_valid=1;
    }
#endif
    /* Recorded from the same resolved value the compiler was given, so the
     * native create path can refuse a pipeline that asks to link this pair for
     * a different primitive. */
    p->primitive_type=options.primitive_type;
    *out=p;
    return VK_SUCCESS;
failed:
    ps5vk_runtime_graphics_diag_result=(int)result;
#if defined(PS5VK_GEOMETRY_KEY_DIAG) && PS5VK_GEOMETRY_KEY_DIAG
    ps5log_printf(PS5LOG_MARK,
        "PS5VK_GEOMETRY_COMPILE_FAIL result=%s(%d) failure=%d "
        "fs=%u hull=%u domain=%u tess=%u hull_wg=%u",
        psbc_result_string(result),(int)result,(int)failure,
        (unsigned)(p->fragment.machine_code!=NULL),
        (unsigned)(p->hull.machine_code!=NULL),
        (unsigned)(p->domain.machine_code!=NULL),
        (unsigned)ps5vk_graphics_has_tessellation(key),
        (unsigned)p->hull.metadata.hull_tess_wg_valid);
#endif
    if(result==PSBC_RESULT_OUT_OF_MEMORY)failure=VK_ERROR_OUT_OF_HOST_MEMORY;
    ps5vk_runtime_graphics_free(NULL,p);
    return failure;
}
