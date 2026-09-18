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
/* The user-config bank addresses registers by (address - 0x30000)/4. Naming
 * the register address and converting here keeps a raw address from being
 * mistaken for an offset, which is what silently dropped the tessellation
 * ring state. */
#define PS5VK_UC_OFFSET(address) ((uint16_t)(((address)-0x30000u)/4u))

enum {
    PS5VK_TESS_OFFCHIP_WORKGROUPS = 144u,
    PS5VK_TESS_OFFCHIP_WORKGROUP_DWORDS = 8192u,
    PS5VK_TESS_OFFCHIP_BYTES =
        PS5VK_TESS_OFFCHIP_WORKGROUPS*PS5VK_TESS_OFFCHIP_WORKGROUP_DWORDS*4u,
    PS5VK_TESS_FACTOR_BYTES = (192u/3u)*16u*3u*18u*2u,
    /* The ring descriptor table the tessellation shaders dereference:
     * sixteen bytes per ring entry, holding an audited raw buffer SRD (the
     * descriptor encoder's byte-addressed word set: base low/high, byte
     * extent, the raw format word). RING_HS_TESS_FACTOR 5 and
     * RING_HS_TESS_OFFCHIP 6 are the entries the stages load; every other
     * entry stays zero. The table sits in front of the rings, 256B aligned,
     * so one allocation carries the table and both rings. */
    PS5VK_TESS_RING_TABLE_BYTES = 256u,
    PS5VK_TESS_OFFCHIP_OFFSET = PS5VK_TESS_RING_TABLE_BYTES,
    PS5VK_TESS_FACTOR_OFFSET = PS5VK_TESS_OFFCHIP_OFFSET+PS5VK_TESS_OFFCHIP_BYTES,
    PS5VK_TESS_RING_INDEX_HS_TESS_FACTOR = 5u,
    PS5VK_TESS_RING_INDEX_HS_TESS_OFFCHIP = 6u,
    PS5VK_TESS_RING_SRD_FORMAT = 0x31016facu,
    PS5VK_TESS_RING_BYTES =
        (PS5VK_TESS_FACTOR_OFFSET+PS5VK_TESS_FACTOR_BYTES+0xffffu)&~0xffffu
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
    struct ps5vk_runtime_shader hull;
    if(ps5vk_runtime_shader_build(&check,
            has_tessellation?&input->domain:&input->vertex) ||
       ps5vk_runtime_shader_build(&check,&input->fragment) ||
       ps5vk_runtime_draw_abi_build(
           has_tessellation?&input->domain.metadata:&input->vertex.metadata,
           &input->fragment.metadata,&arguments))
        {TESS_CREATE_FAIL("abi");return VK_ERROR_FEATURE_NOT_PRESENT;}
    if(has_tessellation && ps5vk_runtime_hull_build(&hull,&input->hull))
        {TESS_CREATE_FAIL("hull");return VK_ERROR_FEATURE_NOT_PRESENT;}
    size_t vs_at=(sizeof(struct ps5vk_graphics_pair)+255u)&~(size_t)255u;
    size_t fs_at=(vs_at+(has_tessellation?input->domain:input->vertex).machine_code_size+
        255u)&~(size_t)255u;
    /* One merged hull image, so one 256B-aligned slot for it. */
    size_t hull_at=(fs_at+input->fragment.machine_code_size+255u)&~(size_t)255u;
    size_t table_at=(has_tessellation?
        hull_at+input->hull.machine_code_size:fs_at+
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
        /* The hull view is appended to the prepared pair: its pointers are
         * field-relative already, so a straight copy keeps them valid. */
        pair->runtime_hull=hull;
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
        /* DIAGNOSTIC BISECT, not a promoted value. NUM_PATCHES is the
         * compiler's per-workgroup CAPACITY (64 for these fixtures) and the
         * handoff froze it absent primary-source proof. The measured failure
         * class has since changed from "fault" to "hang": the draw logs its
         * suspend point, goes silent for ~1.5 s and is killed, which is a
         * launch that never retires rather than a bad address. A workgroup
         * sized for 64 patches fed a single patch is exactly that shape if the
         * geometry engine batches patches before launching, so this switch
         * lets one run test it. Default OFF: the shipped value is unchanged. */
        uint32_t num_patches=input->hull.metadata.hull_num_patches_per_wg;
#if defined(PS5VK_TESS_PATCHES_PER_WG) && PS5VK_TESS_PATCHES_PER_WG
        num_patches=PS5VK_TESS_PATCHES_PER_WG;
#endif
        pair->tess_state[1]=(ps5_agc_register){0x2d6,
            (num_patches&255u) |
            ((input->patch_control_points&63u)<<8) |
            ((input->patch_control_points&63u)<<14)};
        /* The merged hull program is loaded here, so its ONE address register
         * pair takes the real address. On GFX10 that register is the LS block
         * (R_00B520/R_00B524, sh 0x148/0x149) per the pinned
         * radv_get_shader_regs(); the loader already refused a package that
         * published the pre-GFX9 HS address register instead. */
        void *hull_code=(unsigned char *)address+hull_at;
        memcpy(hull_code,input->hull.machine_code,input->hull.machine_code_size);
        const uint64_t hull_va=(uintptr_t)hull_code;
        if(hull_va&255u){rc=VK_ERROR_MEMORY_MAP_FAILED;goto failed;}
        if(pair->runtime_hull.shader[0].offset!=0x148 ||
           pair->runtime_hull.shader[1].offset!=0x149) {
            TESS_CREATE_FAIL("hull-pgm");
            rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
        }
        pair->runtime_hull.shader[0].value=(uint32_t)(hull_va>>8);
        pair->runtime_hull.shader[1].value=(uint32_t)((hull_va>>40)&255u);
        /* The merged LS/HS workgroup's LDS allocation. The compiler CANNOT
         * publish it: the combined RSRC1/RSRC2 pair comes from the pinned
         * radv_shader_combine_cfg_vs_tcs(), which merges VGPR/SGPR counts and
         * ORs the control half's RSRC2 bits but never writes an LDS_SIZE,
         * because the size depends on the patch count - a PIPELINE property.
         * The measured combined value is 0x00000006 (SCRATCH_EN 0, USER_SGPR
         * 3, LDS_SIZE 0), while the same compile publishes
         * hull_tcs_lds_size = 16400 bytes for this fixture. Launching a hull
         * that stages its control points and tess factors in LDS with a
         * zero-sized LDS allocation is what faults the GPU on the first patch
         * draw, whatever the tessellation levels are, which is exactly the
         * measured signature: the level-2 control and the zero-level control
         * die identically while the geometry cases in the same process pass.
         *
         * The encoding is the pinned ac_shader_encode_lds_size(): round the
         * byte count up to the allocation granularity, then divide by the
         * encode granularity. psbc compiles PSBC_TARGET_PS5 as GFX10_3 /
         * CHIP_NAVI21 (libpsbc/psbc_compile.c setup_target), so the
         * allocation granularity is 1024 and the encode granularity is 512.
         * The field is LDS_SIZE at bits 18..26 of SPI_SHADER_PGM_RSRC2_HS
         * (S_00B42C_LDS_SIZE_GFX10), reached at sh offset 0x10b. */
        {
            const uint32_t lds_bytes=input->hull.metadata.hull_tcs_lds_size;
            const uint32_t granules=(lds_bytes+1023u)/1024u;      /* alloc 1024 */
            const uint32_t encoded=granules*2u;                    /* /512 */
            /* 64 KiB of LDS per workgroup is the device maximum and the field
             * is nine bits; refuse rather than truncate into a wrong size. */
            if(!lds_bytes || lds_bytes>65536u || encoded>511u) {
                TESS_CREATE_FAIL("hull-lds");
                rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
            }
            unsigned patched=0;
            for(unsigned i=0;i<pair->runtime_hull.header.num_sh_registers;++i)
                if(pair->runtime_hull.shader[i].offset==0x10b) {
                    pair->runtime_hull.shader[i].value|=encoded<<18;
                    ++patched;
                }
            if(patched!=1) {
                TESS_CREATE_FAIL("hull-rsrc2");
                rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
            }
        }
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
        /* The ring descriptor table: every entry zeroed, then the two rings
         * the tessellation stages read get their audited raw buffer SRDs
         * with the exact base and byte extent. */
        const uint64_t rings_va=(uintptr_t)pair->tess_rings;
        const uint64_t offchip_va=rings_va+PS5VK_TESS_OFFCHIP_OFFSET;
        const uint64_t tf_va=rings_va+PS5VK_TESS_FACTOR_OFFSET;
        {
            const uint64_t bases[2]={tf_va,offchip_va};
            const uint32_t extents[2]={PS5VK_TESS_FACTOR_BYTES,
                PS5VK_TESS_OFFCHIP_BYTES};
            const unsigned indices[2]={PS5VK_TESS_RING_INDEX_HS_TESS_FACTOR,
                PS5VK_TESS_RING_INDEX_HS_TESS_OFFCHIP};
            uint32_t *ring_table=(uint32_t *)pair->tess_rings;
            for(unsigned r=0;r<2;++r) {
                uint32_t *entry=ring_table+indices[r]*4;
                entry[0]=(uint32_t)bases[r];
                entry[1]=(uint32_t)(bases[r]>>32);
                entry[2]=extents[r];
                entry[3]=PS5VK_TESS_RING_SRD_FORMAT;
            }
        }
        /* The table's address, and the hull user-data dword that carries it.
         * The hull dereferences the table (its first memory operation is an
         * SMEM load of the tess-factor ring descriptor at table + 5*16), and
         * on this platform it has to arrive as user data: the system-block
         * ring_offsets at s0/s1 is below the merged program's user-data
         * window and nothing here configures a global ring table. */
        pair->tess_ring_table_low=(uint32_t)rings_va;
        pair->tess_ring_table_high=(uint32_t)(rings_va>>32);
        if(!input->hull.metadata.ps5_ring_table_valid ||
           input->hull.metadata.ps5_ring_table_user_data_dword+1u>=
               input->hull.metadata.user_sgpr_count) {
            TESS_CREATE_FAIL("hull-ring-slot");
            rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
        }
        pair->tess_ring_table_slot=
            input->hull.metadata.ps5_ring_table_user_data_dword;
        /* The tessellation ring device state, in the USER-CONFIG bank.
         *
         * These four registers carry OFFSETS in that bank's index space,
         * (address - 0x30000)/4, exactly like the primitive-restart enable the
         * draw state writes at 0x24b for R_03092C. They previously carried the
         * RAW ADDRESSES 0x30938/0x3093c/0x30940/0x30984, which are not offsets
         * at all: the tess-factor ring base, its size and the off-chip
         * parameter therefore never reached their registers, and four writes
         * landed at out-of-range indices instead. A hull that writes tess
         * factors into a ring whose base register was never programmed faults
         * on any patch draw at any tessellation level - including a
         * zero-level patch, because the control half still writes the factors
         * before the patch is discarded.
         *
         * The values follow the pinned emitter (ac_cmdbuf_cp.c, the GFX7+
         * branch): SIZE is the factor ring in DWORDS, then the off-chip
         * parameter, then the factor ring base >> 8, then its high bits
         * >> 40. S_03093C: OFFCHIP_BUFFERING_GFX103(workgroups-1) in bits
         * 0..9 and the 8K-dword granularity enum 0 in bits 10..11. */
        pair->tess_ring_state[0]=(ps5_agc_register){
            PS5VK_UC_OFFSET(0x030938u),           /* VGT_TF_RING_SIZE */
            (PS5VK_TESS_FACTOR_BYTES/4u)&0x1ffffu};
        pair->tess_ring_state[1]=(ps5_agc_register){
            PS5VK_UC_OFFSET(0x03093Cu),           /* VGT_HS_OFFCHIP_PARAM */
            (PS5VK_TESS_OFFCHIP_WORKGROUPS-1u)&1023u};
        pair->tess_ring_state[2]=(ps5_agc_register){
            PS5VK_UC_OFFSET(0x030940u),           /* VGT_TF_MEMORY_BASE */
            (uint32_t)(tf_va>>8)};
        pair->tess_ring_state[3]=(ps5_agc_register){
            PS5VK_UC_OFFSET(0x030984u),           /* VGT_TF_MEMORY_BASE_HI */
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
