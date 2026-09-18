#include "runtime_graphics_compiler.h"
#include "graphics_pipeline_ps5.h"
#include "graphics_program.h"
#include <stdlib.h>
#include <string.h>
#if defined(PS5VK_GEOMETRY_KEY_DIAG) && PS5VK_GEOMETRY_KEY_DIAG
#include "ps5log.h"
#define TESS_CREATE_FAIL(stage) ps5log_printf(PS5LOG_MARK, \
    "PS5VK_TESS_CREATE_FAIL stage=" stage)
#else
#define TESS_CREATE_FAIL(stage) ((void)0)
#endif

/* The tessellation ring sizing, from the pinned radv device-topology formula
 * (ac_gpu_info.c) with the GFX1013 console's measured engine topology: two
 * shader engines, one shader array each, eighteen good compute units per
 * array. The offchip workgroup allocation is the 8K-dword granularity the
 * hardware names, the workgroup count the OFFCHIP_BUFFERING field bounds at
 * 128 per engine, and the tess-factor ring the typical full-triangle factor
 * size per workgroup times three workgroups per CU. The patch draw writes the
 * whole set as explicit UC state, so a stale ring config from another queue
 * cannot leak in. The ring block backs the offchip workgroups first and the
 * tess-factor ring behind them, and the TF ring base follows the offchip ring
 * exactly as the pinned emitter computes it (ac_cmdbuf_cp.c). */
enum {
    PS5VK_TESS_OFFCHIP_WORKGROUPS = 144u,
    PS5VK_TESS_OFFCHIP_WORKGROUP_DWORDS = 8192u,
    PS5VK_TESS_OFFCHIP_BYTES =
        PS5VK_TESS_OFFCHIP_WORKGROUPS*PS5VK_TESS_OFFCHIP_WORKGROUP_DWORDS*4u,
    PS5VK_TESS_FACTOR_BYTES = (192u/3u)*16u*3u*18u*2u,
    PS5VK_TESS_RING_BYTES =
        (PS5VK_TESS_OFFCHIP_BYTES+PS5VK_TESS_FACTOR_BYTES+0xffffu)&~0xffffu
};

VkResult ps5vk_native_runtime_graphics_create(VkDevice d,const void *data,
    uint32_t primitive_type,void **out)
{
    if(!out)return VK_ERROR_UNKNOWN;
    *out=NULL;
    const struct ps5vk_runtime_graphics_program *input=data;
    if(!d || !input || !d->memory.allocate || !d->memory.release || !d->memory.flush)
        return VK_ERROR_INITIALIZATION_FAILED;
    /* The linker below programs this value as VGT_PRIMITIVE_TYPE for the pair,
     * and the pair's shaders were compiled for the primitive it recorded. A
     * caller that asks for a different primitive is refused before any
     * allocation or linking rather than producing a pair whose compiled code
     * and linked state describe different primitives, and the value itself has
     * to be one this profile resolves from a topology. A tessellation
     * pipeline's draw feeds DI_PT_PATCH (9), which the AGC linker does not
     * accept, so the hull/domain path links the domain as the triangle
     * producer the tessellator's output topology generates and overrides the
     * linked UC primitive afterwards. */
    const int has_tessellation=input->hull.machine_code!=NULL;
    if(primitive_type!=input->primitive_type ||
        (has_tessellation ?
            (primitive_type!=9u || !input->domain.machine_code) :
            !ps5vk_agc_primitive_linkable(primitive_type)))
        {TESS_CREATE_FAIL("primitive");return VK_ERROR_FEATURE_NOT_PRESENT;}
    struct ps5vk_runtime_shader check;
    struct ps5vk_runtime_draw_abi arguments;
    /* The domain half is the pre-raster program a tessellation pipeline
     * packages: the same NGG pre-raster shape a vertex program has. The hull's
     * two program views come from its own builder, whose remaining refusal is
     * the launch state this path prepares below. */
    struct ps5vk_runtime_shader hull_ls,hull_hs;
    if(ps5vk_runtime_shader_build(&check,
            has_tessellation?&input->domain:&input->vertex) ||
       ps5vk_runtime_shader_build(&check,&input->fragment) ||
       ps5vk_runtime_draw_abi_build(
           has_tessellation?&input->domain.metadata:&input->vertex.metadata,
           &input->fragment.metadata,&arguments))
        {TESS_CREATE_FAIL("abi");return VK_ERROR_FEATURE_NOT_PRESENT;}
    if(has_tessellation &&
       ps5vk_runtime_hull_build(&hull_ls,&hull_hs,&input->hull))
        {TESS_CREATE_FAIL("hull");return VK_ERROR_FEATURE_NOT_PRESENT;}
    size_t vs_at=(sizeof(struct ps5vk_graphics_pair)+255u)&~(size_t)255u;
    size_t fs_at=(vs_at+(has_tessellation?input->domain:input->vertex).machine_code_size+
        255u)&~(size_t)255u;
    size_t hs_at=(fs_at+input->fragment.machine_code_size+255u)&~(size_t)255u;
    size_t ls_at=has_tessellation?
        (hs_at+input->hull.metadata.hull_ls_code_offset+255u)&~(size_t)255u:fs_at;
    size_t table_at=(has_tessellation?
        ls_at+input->hull.metadata.hull_ls_code_size:fs_at+
        input->fragment.machine_code_size+15u)&~(size_t)15u;
    struct ps5vk_native_graphics_pipeline *p=calloc(1,sizeof(*p));
    if(!p)return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->device=d;p->memory=d->memory;p->allocation_bytes=table_at+16;
    void *address=NULL;
    VkResult rc=p->memory.allocate(p->memory.context,p->allocation_bytes,&address,&p->backing);
    if(rc!=VK_SUCCESS){free(p);return rc;}
    if(!address || !p->backing || ((uintptr_t)address&255u) ||
       ((uintptr_t)address>>32)!=2 || p->allocation_bytes>UINT64_C(0x300000000)-(uintptr_t)address) {
        rc=VK_ERROR_MEMORY_MAP_FAILED;goto failed;
    }
    memset(address,0,p->allocation_bytes);
    p->pair=address;p->global_table=(uint32_t *)((unsigned char *)address+table_at);
    struct ps5vk_graphics_pair *pair=p->pair;
    pair->runtime_arguments=arguments;
    if(ps5vk_runtime_shader_build(&pair->runtime_vertex,
           has_tessellation?&input->domain:&input->vertex) ||
       ps5vk_runtime_shader_build(&pair->runtime_fragment,&input->fragment)) {
        TESS_CREATE_FAIL("pair-headers");
        rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
    }
    if(has_tessellation) {
        /* The hull views are appended to the prepared pair: their pointers are
         * field-relative already, so a straight copy keeps them valid. */
        pair->runtime_hull_ls=hull_ls;
        pair->runtime_hull_hs=hull_hs;
    }
    void *vs_code=(unsigned char *)address+vs_at,*fs_code=(unsigned char *)address+fs_at;
    memcpy(vs_code,(has_tessellation?input->domain:input->vertex).machine_code,
        (has_tessellation?input->domain:input->vertex).machine_code_size);
    memcpy(fs_code,input->fragment.machine_code,input->fragment.machine_code_size);
    void *vs=NULL,*fs=NULL;
    int32_t agc_rc=sceAgcCreateShader(&vs,&pair->runtime_vertex,vs_code);
    if(!agc_rc && vs==&pair->runtime_vertex) {
        agc_rc=sceAgcCreateShader(&fs,&pair->runtime_fragment,fs_code);
    }
    if(!agc_rc && fs==&pair->runtime_fragment) {
        /* The tessellator generates triangles for the triangle domain, which
         * is the primitive the linked pixel/param state is derived from. */
        agc_rc=sceAgcLinkShaders(&pair->cx,&pair->uc,NULL,vs,fs,
            has_tessellation?PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST:primitive_type);
    }
    if(agc_rc || vs!=&pair->runtime_vertex || fs!=&pair->runtime_fragment) {
        TESS_CREATE_FAIL("agc");
        rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
    }
    if(has_tessellation) {
        /* The draw feeds patches: DI_PT_PATCH from the pinned gfx103 register
         * data overrides the linked triangle value in the UC block, and the
         * hull launch state joins the context block: the stage enables gain
         * the LS/HS halves (the domain's linked value carries ES/PRIMGEN) and
         * the LS_HS_CONFIG carries the compiler's patch-per-workgroup count
         * with the input/output control-point counts the pipeline stated. */
        pair->uc.vgt_primitive_type.value=9u;
        pair->tessellation=1;
        pair->tess_state[0]=(ps5_agc_register){0x2d5,
            input->domain.metadata.linkage_stages_en.value |
            (1u<<0) | /* V_028B54_LS_STAGE_ON */
            (1u<<2)}; /* V_028B54_HS_STAGE_ON */
        pair->tess_state[1]=(ps5_agc_register){0x2d6,
            (input->hull.metadata.hull_num_patches_per_wg&255u) |
            ((input->patch_control_points&63u)<<8) |
            ((input->patch_control_points&63u)<<14)};
        /* The hull programs are loaded here, so their pgm registers take the
         * real addresses now: the HS half sits at offset 0 of the hull machine
         * code and the LS half behind it, both 256B-aligned by construction. */
        void *hs_code=(unsigned char *)address+hs_at,*ls_code=(unsigned char *)address+ls_at;
        memcpy(hs_code,input->hull.machine_code,
            input->hull.metadata.hull_ls_code_offset);
        memcpy(ls_code,(unsigned char *)input->hull.machine_code+
            input->hull.metadata.hull_ls_code_offset,
            input->hull.metadata.hull_ls_code_size);
        const uint64_t hs_va=(uintptr_t)hs_code,ls_va=(uintptr_t)ls_code;
        if((hs_va|ls_va)&255u){rc=VK_ERROR_MEMORY_MAP_FAILED;goto failed;}
        pair->runtime_hull_hs.shader[0].value=(uint32_t)(hs_va>>8);
        pair->runtime_hull_hs.shader[1].value=(uint32_t)((hs_va>>40)&255u);
        pair->runtime_hull_ls.shader[0].value=(uint32_t)(ls_va>>8);
        pair->runtime_hull_ls.shader[1].value=(uint32_t)((ls_va>>40)&255u);
        /* The tessellation rings: one bounded block backing the offchip
         * workgroups and the tess-factor ring behind them, owned by the
         * pipeline and programmed as device state on every patch draw. */
        rc=p->memory.allocate(p->memory.context,PS5VK_TESS_RING_BYTES,
            &pair->tess_rings,&p->rings_backing);
        if(rc!=VK_SUCCESS)goto failed;
        if(!pair->tess_rings || !p->rings_backing ||
           ((uintptr_t)pair->tess_rings&255u) ||
           ((uintptr_t)pair->tess_rings>>32)!=2 ||
           p->allocation_bytes>UINT64_C(0x300000000)-(uintptr_t)pair->tess_rings) {
            rc=VK_ERROR_MEMORY_MAP_FAILED;goto failed;
        }
        memset(pair->tess_rings,0,PS5VK_TESS_RING_BYTES);
        const uint64_t rings_va=(uintptr_t)pair->tess_rings;
        const uint64_t tf_va=rings_va+PS5VK_TESS_OFFCHIP_BYTES;
        /* S_03093C: OFFCHIP_BUFFERING_GFX103(workgroups-1) in bits 0..9 and
         * the 8K-dword granularity enum 0 in bits 10..11, from the pinned
         * register header and the pinned emitter's device derivation. */
        pair->tess_ring_state[0]=(ps5_agc_register){0x30938,
            PS5VK_TESS_FACTOR_BYTES/4u};
        pair->tess_ring_state[1]=(ps5_agc_register){0x3093c,
            (PS5VK_TESS_OFFCHIP_WORKGROUPS-1u)&1023u};
        pair->tess_ring_state[2]=(ps5_agc_register){0x30940,
            (uint32_t)(tf_va>>8)};
        pair->tess_ring_state[3]=(ps5_agc_register){0x30984,
            (uint32_t)((tf_va>>40)&255u)};
        rc=p->memory.flush(p->memory.context,p->rings_backing,0,PS5VK_TESS_RING_BYTES);
        if(rc!=VK_SUCCESS)goto failed;
    }
    pair->vertex_quantization=0x2d;pair->ready=1;
    /* Recorded from the compiled pre-raster program rather than re-derived at
     * draw time: the metadata is freed with the program, and the draw path needs
     * the same fact the descriptor profile used to accept the binding. */
    pair->geometry_preraster=!has_tessellation &&
        input->vertex.metadata.source_stage==PSBC_STAGE_GEOMETRY;
    rc=p->memory.flush(p->memory.context,p->backing,0,p->allocation_bytes);
    if(rc!=VK_SUCCESS)goto failed;
    *out=p;return VK_SUCCESS;
failed:
    if(p->backing)p->memory.release(p->memory.context,p->backing);
    free(p);return rc;
}
