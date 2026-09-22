#include "runtime_graphics_compiler.h"
#include "graphics_pipeline_ps5.h"
#include "graphics_program.h"
#include "tess_entry_witness.h"
#include "tess_shared_storage.h"
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
 * size per workgroup times three workgroups per CU. Emitted UC state alone
 * did not bind the ring consumed by the native tessellator: the successful
 * diagnostic also binds it with sceAgcDriverSetTFRing and verifies the getter.
 * The queue owns binding and shares storage across pipelines independently
 * of diagnostic logging switches.
 * General capability promotion remains pending. The block backs
 * the offchip workgroups first and the
 * tess-factor ring behind them, and the TF ring base follows the offchip ring
 * exactly as the pinned emitter computes it (ac_cmdbuf_cp.c). */
/* The user-config bank addresses registers by (address - 0x30000)/4. Naming
 * the register address and converting here keeps a raw address from being
 * mistaken for an offset, which is what silently dropped the tessellation
 * ring state. */
#ifndef PS5VK_TESS_LINK_PRIMITIVE
#define PS5VK_TESS_LINK_PRIMITIVE PS5VK_AGC_PRIMITIVE_TYPE_TRIANGLE_LIST
#endif
#ifndef PS5VK_TESS_DISTRIBUTION_MODE
#define PS5VK_TESS_DISTRIBUTION_MODE 3 /* V_028B6C_TRAPEZOIDS */
#endif
#define PS5VK_UC_OFFSET(address) ((uint16_t)(((address)-0x30000u)/4u))
#ifndef PS5VK_TESS_OFFCHIP_CAPACITY_WG
#define PS5VK_TESS_OFFCHIP_CAPACITY_WG 256u
#endif
/* Default to the native-witnessed256 workgroups. An explicit diagnostic
 * capacity changes allocation, emitted bounds and checked native binding
 * together. Never infer existing driver state from this requested capacity. */
_Static_assert(PS5VK_TESS_OFFCHIP_CAPACITY_WG>=144u &&
    PS5VK_TESS_OFFCHIP_CAPACITY_WG<=256u,"bounded tess offchip capacity");

enum {
    PS5VK_TESS_OFFCHIP_WORKGROUPS = PS5VK_TESS_OFFCHIP_CAPACITY_WG,
    PS5VK_TESS_OFFCHIP_WORKGROUP_DWORDS = 8192u,
    PS5VK_TESS_OFFCHIP_BYTES =
        PS5VK_TESS_OFFCHIP_CAPACITY_WG*PS5VK_TESS_OFFCHIP_WORKGROUP_DWORDS*4u,
    /* Native getter/setter size remains opaque; the measured raw value65536
     * is backed conservatively in bytes or dwords. Always pair this storage
     * with checked queue binding, independently of diagnostic logging. */
    PS5VK_TESS_FACTOR_BYTES = 65536u*4u,
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

static VkResult initialize_tess_storage(const struct ps5vk_memory_backend *memory,
    void *address, void *backing, VkDeviceSize bytes)
{
    const uint64_t base=(uintptr_t)address;
    /* Check the RING extent, not the unrelated shader allocation size. */
    if(!address || !backing || (base&255u) || (base>>32)!=2 ||
       bytes!=PS5VK_TESS_RING_BYTES || bytes>UINT64_C(0x300000000)-base)
        return VK_ERROR_MEMORY_MAP_FAILED;
    memset(address,0,(size_t)bytes);
#if defined(PS5VK_TESS_ENTRY_WITNESS) && PS5VK_TESS_ENTRY_WITNESS
    for(unsigned i=48;i<56;++i)((uint32_t *)address)[i]=0xcdcdcdcdu;
#endif
    const uint64_t bases[2]={base+PS5VK_TESS_FACTOR_OFFSET,
                            base+PS5VK_TESS_OFFCHIP_OFFSET};
    const uint32_t extents[2]={PS5VK_TESS_FACTOR_BYTES,PS5VK_TESS_OFFCHIP_BYTES};
    const unsigned indices[2]={PS5VK_TESS_RING_INDEX_HS_TESS_FACTOR,
                              PS5VK_TESS_RING_INDEX_HS_TESS_OFFCHIP};
    for(unsigned r=0;r<2;++r) {
        uint32_t *entry=(uint32_t *)address+indices[r]*4;
        entry[0]=(uint32_t)bases[r];entry[1]=(uint32_t)(bases[r]>>32);
        entry[2]=extents[r];entry[3]=PS5VK_TESS_RING_SRD_FORMAT;
    }
#if defined(PS5VK_TESS_HULL_TRACE) && PS5VK_TESS_HULL_TRACE
    /* Use alignment padding AFTER both rings, never an active factor region. */
    const uint64_t trace=base+PS5VK_TESS_FACTOR_OFFSET+PS5VK_TESS_FACTOR_BYTES;
    _Static_assert(PS5VK_TESS_RING_BYTES-PS5VK_TESS_FACTOR_OFFSET-
                   PS5VK_TESS_FACTOR_BYTES>=8192u,"owned hull trace extent");
    uint32_t *trace_entry=(uint32_t *)address+28;
    trace_entry[0]=(uint32_t)trace;trace_entry[1]=(uint32_t)(trace>>32);
    trace_entry[2]=8192u;
#endif
    return memory->flush(memory->context,backing,0,bytes);
}

/* VGT_TF_PARAM as the control stage published it: the tessellator's domain,
 * partitioning and output topology, which the hull metadata carries in its
 * context block at cx 0x2db. */
static uint32_t tess_tf_param_value(const PsbcShaderMetadata *m)
{
    for(uint32_t i=0;i<m->context_register_count;++i)
        if(m->context_registers[i].offset==0x2db)
            return m->context_registers[i].value;
    return 0u;
}

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
    if(has_tessellation && (!input->patch_control_points ||
       input->patch_control_points>32u || !input->tess_output_points ||
       input->tess_output_points>32u))return VK_ERROR_FEATURE_NOT_PRESENT;
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
    const int fragment_export=ps5vk_runtime_fragment_export(&input->fragment.metadata);
    /* The compiler's own shape and the register class must agree, and the
     * dual-source flag is exactly the DUAL shape: a two-colour-target pair
     * publishes the same register pair but is not dual source, so deriving the
     * flag from the registers alone would mislabel it (and the draw state keys
     * its conversion on that flag). */
    if(fragment_export<0 || input->dual_source_export>1u ||
       input->fragment_shape>PS5VK_RUNTIME_FRAGMENT_SHAPE_TWO_MRT ||
       input->dual_source_export!=(uint32_t)
           (input->fragment_shape==PS5VK_RUNTIME_FRAGMENT_SHAPE_DUAL) ||
       (input->fragment_shape==PS5VK_RUNTIME_FRAGMENT_SHAPE_SINGLE &&
        fragment_export!=PS5VK_RUNTIME_FRAGMENT_EXPORT_SINGLE &&
        fragment_export!=PS5VK_RUNTIME_FRAGMENT_EXPORT_NONE) ||
       (input->fragment_shape!=PS5VK_RUNTIME_FRAGMENT_SHAPE_SINGLE &&
        fragment_export!=PS5VK_RUNTIME_FRAGMENT_EXPORT_DUAL))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if(has_tessellation && ps5vk_runtime_hull_build(&hull,&input->hull))
        {TESS_CREATE_FAIL("hull");return VK_ERROR_FEATURE_NOT_PRESENT;}
    size_t vs_at=(sizeof(struct ps5vk_graphics_pair)+255u)&~(size_t)255u;
    size_t fs_at=(vs_at+(has_tessellation?input->domain:input->vertex).machine_code_size+
        255u)&~(size_t)255u;
    /* One merged hull image, so one 256B-aligned slot for it. */
    size_t hull_at=(fs_at+input->fragment.machine_code_size+255u)&~(size_t)255u;
    size_t hull_prefix_bytes=0;
#if defined(PS5VK_TESS_ENTRY_WITNESS) && PS5VK_TESS_ENTRY_WITNESS
    if(has_tessellation) hull_prefix_bytes=PS5VK_TESS_ENTRY_PREFIX_BYTES;
#endif
#if defined(PS5VK_TESS_HULL_TRACE) && PS5VK_TESS_HULL_TRACE
    if(has_tessellation) hull_prefix_bytes=PS5VK_TESS_ENTRY_PREFIX_BYTES;
#endif
    /* DIAGNOSTIC (PS5VK_TESS_LEGACY_DOMAIN): one more 256B-aligned slot for
     * the legacy hardware-VS image of the evaluation half. */
    const int has_legacy=input->domain_legacy_valid &&
        input->domain_legacy.machine_code && input->domain_legacy.machine_code_size;
    size_t legacy_at=(hull_at+hull_prefix_bytes+(has_tessellation?input->hull.machine_code_size:0)+
        255u)&~(size_t)255u;
    size_t table_at=(has_tessellation?
        (has_legacy?legacy_at+input->domain_legacy.machine_code_size:
         hull_at+hull_prefix_bytes+input->hull.machine_code_size):fs_at+
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
    pair->hull_arguments=input->hull_arguments;
    pair->dual_source_export=input->dual_source_export;
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
        /* WHAT AGC IS TOLD THE PIPELINE DRAWS.
         *
         * This passed TRIANGLE_LIST for a tessellation pipeline, on the
         * reasoning that the tessellator generates triangles and the linked
         * pixel/parameter state is derived from the primitive. The register
         * it produces was then overridden to DI_PT_PATCH afterwards.
         *
         * That reasoning is about the REGISTER, and sceAgcLinkShaders is not
         * a register setter: it derives the whole linked context and
         * user-config state from its arguments, including
         * VGT_SHADER_STAGES_EN and VGT_GS_OUT_PRIM_TYPE. Telling it
         * TRIANGLE_LIST describes a pipeline whose front end assembles
         * triangles, and then correcting one register afterwards leaves
         * anything else the link derived describing that other pipeline -
         * which is the same shape as ES_EN describing a vertex-fed export
         * stage while the enables claimed otherwise.
         *
         * PS5VK_AGC_PRIMITIVE_TYPE_PATCH exists and is the honest argument
         * for a patch draw. It is a knob rather than a change because AGC may
         * refuse it, and a refusal is an ordinary error return the create
         * path already reports. */
        agc_rc=sceAgcLinkShaders(&pair->cx,&pair->uc,NULL,vs,fs,
            has_tessellation?PS5VK_TESS_LINK_PRIMITIVE:primitive_type);
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
         * with independent pipeline input and TCS output control-point counts. */
        pair->uc.vgt_primitive_type.value=9u;
        pair->tessellation=1;
        /* VGT_SHADER_STAGES_EN for a tessellation pipeline.
         *
         * The domain publishes this value for a STANDALONE NGG program, so it
         * says ES_EN = ES_STAGE_REAL: an export stage fed by the vertex
         * fetcher. In a tessellation pipeline the export stage IS the domain
         * shader and it is fed by the TESSELLATOR, which the pinned register
         * data spells as a distinct enumerant - VGT_STAGES_ES_EN is
         * ES_STAGE_OFF 0, ES_STAGE_DS 1, ES_STAGE_REAL 2, and DS is the domain
         * shader case. Leaving REAL there describes a vertex-fed pipeline, and
         * the LS/HS stages and the tessellator are not on that path at all,
         * which is measurably what happens: the hull never reaches its factor
         * store and the draw stalls waiting for work that never flows.
         *
         * So ES_EN is REPLACED rather than ORed - the two enumerants share
         * bits 3..4 and ORing REAL with DS would give the reserved value 3 -
         * and the LS/HS enables are added on top. */
        pair->tess_state[0]=(ps5_agc_register){0x2d5,
            (input->domain.metadata.linkage_stages_en.value & ~(3u<<3)
#if defined(PS5VK_TESS_VS_EN_DS) && PS5VK_TESS_VS_EN_DS
             & ~(3u<<6)
#endif
             ) |
            (1u<<3) | /* V_028B54_ES_STAGE_DS: the tessellator feeds it */
            (1u<<0) | /* V_028B54_LS_STAGE_ON */
#if defined(PS5VK_TESS_VS_EN_DS) && PS5VK_TESS_VS_EN_DS
            /* DIAGNOSTIC BISECT, default off: VS_EN = V_028B54_VS_STAGE_DS.
             *
             * I claimed in the mailbox that every bit of
             * VGT_SHADER_STAGES_EN had been measured in both states. That was
             * wrong. VS_EN at bits 6..7 has never been varied - it has read
             * V_028B54_VS_STAGE_REAL, which is zero, in all fifty-eight runs
             * because nothing ever writes it.
             *
             * It deserves the run it should have had earlier because it is
             * the OTHER "this stage is fed by the tessellator" enumerant in
             * the same register, exactly parallel to ES_EN - and ES_EN
             * reading ES_STAGE_REAL instead of ES_STAGE_DS was a real defect
             * found in this task. On the legacy path the domain shader runs
             * on the VS hardware stage and VS_EN selects VS_STAGE_DS; with
             * NGG the ES stage takes that role through ES_EN, which is why
             * radv leaves VS_EN alone. Whether this hardware also consults it
             * is not something any register dump can answer, and no working
             * pipeline on this device has ever had a tessellator-fed stage to
             * exercise it with.
             *
             * Two bits, replaced rather than ORed: the field is two bits wide
             * and VS_STAGE_COPY_SHADER is 2, so an OR could produce a value
             * that means something else entirely - the same trap ES_EN had.
             *
             * MEASURED AND CLOSED, run 59: VGT_SHADER_STAGES_EN read back
             * 0x0201204d with VS_EN = V_028B54_VS_STAGE_DS, the evaluation
             * half still did not execute, no stall line, 0.157 s. This is not
             * what was missing. It stays default off - it is a diagnostic, and
             * setting a field the hardware did not ask for buys nothing. */
            (1u<<6) | /* V_028B54_VS_STAGE_DS */
#endif
#if defined(PS5VK_TESS_DYNAMIC_HS) && PS5VK_TESS_DYNAMIC_HS
            /* DIAGNOSTIC BISECT, default off, no primary source: radv never
             * sets DYNAMIC_HS and works on this generation under Linux, where
             * the kernel's context initialisation owns whatever this field
             * needs to be.
             *
             * It earns a run because of exactly where the failure now sits.
             * The merged LS/HS program is proven to execute and store correct
             * tessellation factors, for the triangle domain and the isoline
             * domain alike, and the evaluation half is proven by two
             * independent witnesses not to execute. So the geometry engine
             * dispatches the hull and then does not run the tessellator, and
             * this is the only bit left in VGT_SHADER_STAGES_EN that names
             * the HS dispatch mode rather than a stage enable. */
            (1u<<8) | /* DYNAMIC_HS */
#endif
#if defined(PS5VK_TESS_GS_EN) && PS5VK_TESS_GS_EN
            /* DIAGNOSTIC BISECT, default off, and deliberately NOT what radv
             * does: it sets GS_EN only for a real API geometry shader.
             *
             * It earns one run because of the one structural difference
             * between this pipeline and every NGG pipeline that PASSES on
             * this device. The geometry probe's merged pair runs with
             * ES_EN = ES_STAGE_REAL and GS_EN = 1; its plain vertex cases run
             * with ES_STAGE_REAL and GS_EN = 0. Ours is the only combination
             * on this device that pairs ES_STAGE_DS with GS_EN = 0, and
             * nothing here has ever exercised a tessellator-fed export stage
             * before. If the geometry engine needs the GS stage enabled to
             * schedule a DS-fed NGG subgroup, no value of any other register
             * could show it. */
            (1u<<5) | /* V_028B54_GS_STAGE_ON */
#endif
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
            ((input->tess_output_points&63u)<<14)};
        /* VGT_GS_OUT_PRIM_TYPE, corrected for the tessellator.
         *
         * The linked value comes from the domain compiled as a STANDALONE NGG
         * program, and it came back 0 = V_028A6C_POINTLIST, measured directly
         * off the linked context block. The working geometry pipeline runs
         * with 2 = V_028A6C_TRISTRIP. A pre-raster stage that tells the
         * geometry engine it emits POINTS while the tessellator is generating
         * triangles describes a pipeline that does not exist, and the engine
         * has no triangle work to schedule - which is the same
         * compiled-in-isolation mistake as the LS half, the domain's patch
         * count and ES_EN.
         *
         * Derived from the tessellator's own configuration rather than
         * assumed: VGT_TF_PARAM's TYPE says isolines, triangles or quads, and
         * its TOPOLOGY says point mode. Isolines emit line strips, point mode
         * emits points, everything else emits triangle strips. */
        {
            const uint32_t tf=tess_tf_param_value(&input->hull.metadata);
            const uint32_t tf_type=tf&3u,tf_topology=(tf>>5)&7u;
            uint32_t out_prim=2u;                 /* V_028A6C_TRISTRIP */
            if(tf_topology==0u)out_prim=0u;       /* OUTPUT_POINT */
            else if(tf_type==0u)out_prim=1u;      /* TESS_ISOLINE */
            /* TES determines the GS input, not its output. With a merged
             * TES+GS program the compiler's GS declaration is authoritative;
             * AGC's generic patch linking must not replace it with TES type. */
            if(input->domain.metadata.source_stage==PSBC_STAGE_GEOMETRY) {
                unsigned found=0;
                for(unsigned i=0;i<input->domain.metadata.context_register_count;++i)
                    if(input->domain.metadata.context_registers[i].offset==0x29b) {
                        out_prim=input->domain.metadata.context_registers[i].value;
                        ++found;
                    }
                if(found!=1 || out_prim>2u) {
                    TESS_CREATE_FAIL("domain-geometry-output-primitive");
                    rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
                }
            }
            pair->cx.vgt_gs_out_prim_type=
                (ps5_agc_register){0x29b,out_prim};
        }
        /* VGT_TESS_DISTRIBUTION. The pinned emitter programs this for every
         * GFX8+ device as part of its initialisation (ac_cmdbuf_cp.c, the
         * non-GFX9 branch): ACCUM_ISOLINE 32, ACCUM_TRI 11, ACCUM_QUAD 11,
         * DONUT_SPLIT 16, and TRAP_SPLIT 3 for CHIP_FIJI or >= CHIP_POLARIS10,
         * which CHIP_NAVI21 is. It tells the geometry engine how to distribute
         * tessellation work, and this driver has never written it at all -
         * it was read as device state the native graphics API would own.
         *
         * That assumption was never tested, and the failure it would produce
         * is a STALL rather than a fault, which is what the measured silence
         * before the watchdog kill now says this is. Written with the rest of
         * the tessellation context, so nothing depends on what another
         * pipeline left configured. Field widths are the pinned header's:
         * three 8-bit accumulators, then DONUT_SPLIT 5 bits at 24 and
         * TRAP_SPLIT 3 bits at 29 on this generation. */
        pair->tess_state[2]=(ps5_agc_register){0x2d4,
            (32u&0xffu) | ((11u&0xffu)<<8) | ((11u&0xffu)<<16) |
            ((16u&0x1fu)<<24) | ((3u&0x7u)<<29)};
        /* THE HARDWARE'S TESSELLATION-LEVEL CLAMPS, which this driver has
         * never written at all.
         *
         * VGT_HOS_MAX_TESS_LEVEL and VGT_HOS_MIN_TESS_LEVEL bound every
         * tessellation factor the geometry engine reads, as IEEE floats. The
         * pinned emitter sets both as part of device initialisation - three
         * separate init paths in ac_cmdbuf.c all write
         * R_028A18_VGT_HOS_MAX_TESS_LEVEL = fui(64) and
         * R_028A1C_VGT_HOS_MIN_TESS_LEVEL = fui(0) - and this driver performs
         * no such initialisation, exactly as it performed none for
         * VGT_TESS_DISTRIBUTION or the tessellation ring's user-config
         * registers, both of which had to be added here for the same reason.
         *
         * The consequence if the maximum holds zero is total and silent: every
         * factor the hull writes is clamped to 0.0, the tessellator generates
         * nothing, no domain waves launch, no primitive reaches the rasteriser
         * and the draw retires normally with an empty image. That is precisely
         * the measured state - the hull proven to store outer 2.0, 2.0, 2.0 and
         * inner 1.0, a witness reporting the evaluation half ran ZERO times,
         * ink=0 and foreign=0, and a clean 0.168 s draw.
         *
         * Written with the rest of the tessellation context on every patch
         * draw, so nothing depends on what another pipeline or the platform's
         * own initialisation left behind. 64.0f is 0x42800000 and 0.0f is
         * zero; they are spelled as the bit patterns the register takes rather
         * than built through a float cast, so the emitted value is readable
         * next to the pinned fui(64) it comes from. */
        pair->tess_state[3]=(ps5_agc_register){0x286,0x42800000u};
        pair->tess_state[4]=(ps5_agc_register){0x287,0x00000000u};
        /* DIAGNOSTIC BISECT, default off.
         *
         * VGT_GS_MAX_VERT_OUT bounds how many vertices the NGG stage may
         * emit. psbc publishes gs.vertices_out for it, which is ZERO for any
         * stage that is not an API geometry shader - so both this pipeline
         * and the PASSING vertex NGG draws run with zero there, measured in
         * both.
         *
         * The reason it is worth a run anyway is the asymmetry that has been
         * the shape of this whole problem: for a VERTEX-fed NGG pipeline the
         * geometry engine knows the vertex count from the draw itself, so a
         * zero bound costs nothing. For a TESSELLATOR-fed one the vertex
         * count comes out of the tessellator, and a bound of zero is a
         * legitimate reading of "emit at most nothing" - which is exactly
         * what is measured: the hull executes, its factors are correct, and
         * the evaluation half never runs. Every other register that passed
         * the "the working draw has it too" test passed it fairly; this one
         * passes it only because the working draw never needs the field.
         *
         * The candidate value is the subgroup's own output limit, which this
         * pipeline already publishes at GE_MAX_OUTPUT_PER_SUBGROUP, rather
         * than a number chosen to be large. */
#if defined(PS5VK_TESS_GS_MAX_VERT_OUT) && PS5VK_TESS_GS_MAX_VERT_OUT
        {
            unsigned patched_mvo=0;
            for(unsigned i=0;i<input->domain.metadata.context_register_count;++i)
                if(input->domain.metadata.context_registers[i].offset==0x2ce)
                    ++patched_mvo;
            if(patched_mvo!=1) {
                TESS_CREATE_FAIL("domain-max-vert-out");
                rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
            }
            for(unsigned i=0;i<pair->runtime_vertex.header.num_cx_registers;++i)
                if(pair->runtime_vertex.context[i].offset==0x2ce)
                    pair->runtime_vertex.context[i].value=
                        (PS5VK_TESS_GS_MAX_VERT_OUT & 0x7ffu);
        }
#endif
        /* VGT_TF_PARAM.DISTRIBUTION_MODE, paired with the register above.
         *
         * The accumulators this driver writes at 0x2d4 only mean anything
         * when the tessellator is actually distributing work, and the pinned
         * tree says so in its own words: radv_pipeline_graphics.c carries the
         * comment "Needed for 028B6C_DISTRIBUTION_MODE != 0" over the
         * has_distributed_tess branch. ac_gpu_info.c sets
         * has_distributed_tess for every gfx_level >= GFX10, so this device
         * is one of them, and radv therefore runs CHIP_NAVI21 with the
         * accumulators AND a non-zero distribution mode.
         *
         * We were programming the accumulators with DISTRIBUTION_MODE left
         * at V_028B6C_NO_DIST, because the mode lives in VGT_TF_PARAM, which
         * psbc derives from the control half's declared interface alone -
         * domain, spacing and output topology - and a shader interface knows
         * nothing about how the hardware distributes patches. That is a
         * pairing radv never produces, and it is the same
         * compiled-in-isolation shape as every other defect in this pipeline:
         * a pipeline-level decision taken from a stage that cannot make it.
         *
         * V_028B6C_TRAPEZOIDS is 3 in the pinned header, not 2; the
         * enumerant list is NO_DIST 0, PATCHES 1, DONUTS 2, TRAPEZOIDS 3.
         * Patched into the hull's own published context register so the draw
         * keeps emitting exactly one VGT_TF_PARAM. */
        {
            unsigned patched_tf=0;
            for(unsigned i=0;i<pair->runtime_hull.header.num_cx_registers;++i)
                if(pair->runtime_hull.context[i].offset==0x2db) {
                    /* The distribution mode is a KNOB rather than a
                     * constant, because its first measurement is not valid.
                     * It was set to TRAPEZOIDS on a consistency argument -
                     * radv runs this chip with the accumulators and a
                     * non-zero mode - and measured to "change nothing", but
                     * that run happened BEFORE the hull stopped storing
                     * quad-shaped tessellation factors, so nothing downstream
                     * could have worked whatever this field held. A null
                     * result measured upstream of a known defect is not a
                     * null result. Default 3 is the current shipped value;
                     * 0 is V_028B6C_NO_DIST, what the compiler published
                     * before this session touched it. */
                    pair->runtime_hull.context[i].value=
                        (pair->runtime_hull.context[i].value & ~(3u<<17)) |
                        ((PS5VK_TESS_DISTRIBUTION_MODE & 3u)<<17);
                    /* DIAGNOSTIC BISECT, not a promoted value. Default 0
                     * leaves the shipped behaviour untouched.
                     *
                     * VGT_TF_PARAM is written WHOLE from a value psbc derives
                     * from the control half's declared interface - domain,
                     * spacing, topology - so every other field in the
                     * register is zeroed, including NUM_DS_WAVES_PER_SIMD at
                     * bits 10..13. The pinned header says that field exists
                     * on gfx10 and gfx103. Nothing in the pinned tree derives
                     * it, and radv on Linux never writes it either, which
                     * means on Linux it keeps whatever the kernel's context
                     * initialisation left there - a default this driver
                     * destroys, because the register is not in the linked AGC
                     * block and nothing restores it.
                     *
                     * That matters here only because of what is now measured:
                     * the hull executes, stores correct triangle-layout
                     * factors, and NOTHING downstream appears. A field that
                     * bounds how many domain-shader waves a SIMD may run is
                     * the one field in this register that could produce
                     * exactly that if zero means none rather than unlimited.
                     * It is a HYPOTHESIS with no primary source, so it gets a
                     * switch and one run rather than a promotion. */
#if defined(PS5VK_TESS_DS_WAVES) && PS5VK_TESS_DS_WAVES
                    pair->runtime_hull.context[i].value=
                        (pair->runtime_hull.context[i].value & ~(15u<<10)) |
                        ((PS5VK_TESS_DS_WAVES & 15u)<<10);
#endif
                    /* DIAGNOSTIC BISECT, default 0 (V_028B6C_VGT_POLICY_LRU,
                     * the shipped behaviour).
                     *
                     * RDREQ_POLICY at bits 15..16 selects how the geometry
                     * engine's tessellation-factor READS are cached, and this
                     * driver zeroes it along with every other field it does
                     * not derive. The pinned header names the alternatives:
                     * VGT_POLICY_LRU 0, VGT_POLICY_STREAM 1 and
                     * VGT_POLICY_BYPASS 2, the last valid on gfx10 and
                     * gfx103.
                     *
                     * It earns a run because of what the measurements have
                     * narrowed to. The hull writes outer 2.0, 2.0, 2.0 and
                     * inner 1.0 and they are in memory, verified by reading
                     * the ring. The domain never launches. And on this
                     * generation DETECT_ZERO=0 means PRE_CLAMP_TF0 - the
                     * hardware CULLS a patch whose tessellation factor reads
                     * as zero, tested before clamping. Those three facts are
                     * consistent if the VGT is not reading the bytes the hull
                     * wrote, and BYPASS is the enumerant whose name is that
                     * hypothesis rather than a numeric guess at it. */
#if defined(PS5VK_TESS_TF_RDREQ) && PS5VK_TESS_TF_RDREQ
                    pair->runtime_hull.context[i].value=
                        (pair->runtime_hull.context[i].value & ~(3u<<15)) |
                        ((PS5VK_TESS_TF_RDREQ & 3u)<<15);
#endif
                    ++patched_tf;
                }
            if(patched_tf!=1) {
                TESS_CREATE_FAIL("hull-tf-param");
                rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
            }
        }
        /* The merged hull program is loaded here, so its ONE address register
         * pair takes the real address. On GFX10 that register is the LS block
         * (R_00B520/R_00B524, sh 0x148/0x149) per the pinned
         * radv_get_shader_regs(); the loader already refused a package that
         * published the pre-GFX9 HS address register instead. */
        void *hull_code=(unsigned char *)address+hull_at;
#if defined(PS5VK_TESS_HULL_TRACE) && PS5VK_TESS_HULL_TRACE
        if(hull_prefix_bytes && ps5vk_tess_hull_trace_prefix(hull_code,hull_prefix_bytes,
                input->hull.metadata.ps5_ring_table_user_data_dword,
                input->hull.metadata.user_sgpr_count)) {
#else
        if(hull_prefix_bytes && ps5vk_tess_entry_prefix(hull_code,hull_prefix_bytes,
                input->hull.metadata.ps5_ring_table_user_data_dword,
                input->hull.metadata.user_sgpr_count)) {
#endif
            TESS_CREATE_FAIL("entry-witness-slot");
            rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
        }
        memcpy((unsigned char *)hull_code+hull_prefix_bytes,
            input->hull.machine_code,input->hull.machine_code_size);
        pair->runtime_hull.header.shader_size+=hull_prefix_bytes;
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
        /* Sharing is per device, with references retained by pipelines. The
         * pending-use guard forbids their destruction before GPU completion.
         * Never clear/flush an existing ring when a second pipeline is made. */
        rc=ps5vk_tess_storage_acquire(d,&p->memory,PS5VK_TESS_RING_BYTES,
            initialize_tess_storage,&p->shared_rings);
        if(rc!=VK_SUCCESS)goto failed;
        pair->tess_rings=ps5vk_tess_storage_address(p->shared_rings);
        p->rings_backing=ps5vk_tess_storage_backing(p->shared_rings);
        const uint64_t rings_va=(uintptr_t)pair->tess_rings;
        const uint64_t tf_va=rings_va+PS5VK_TESS_FACTOR_OFFSET;
        /* The table's address, and the hull user-data dword that carries it.
         * The hull dereferences the table (its first memory operation is an
         * SMEM load of the tess-factor ring descriptor at table + 5*16), and
         * on this platform it has to arrive as user data: the system-block
         * ring_offsets at s0/s1 is below the merged program's user-data
         * window and nothing here configures a global ring table. */
        pair->tess_ring_table_low=(uint32_t)rings_va;
        pair->tess_ring_table_high=(uint32_t)(rings_va>>32);
        if(pair->hull_arguments.enabled) {
            pair->hull_arguments.ring_table_low=(uint32_t)rings_va;
            pair->hull_arguments.ring_table_high=(uint32_t)(rings_va>>32);
        }
        /* The DOMAIN half's copy of the same pointer. Its metadata declares
         * the table at a window-relative dword and the draw path writes the
         * pre-raster user-data block from these fields, which could not be
         * filled when the block was built because the rings are allocated
         * here. A domain that declares a table and receives zero dereferences
         * NULL the moment it reads a per-vertex or per-patch input. */
        if(pair->runtime_arguments.ring_table_valid) {
            pair->runtime_arguments.ring_table_low=(uint32_t)rings_va;
            pair->runtime_arguments.ring_table_high=(uint32_t)(rings_va>>32);
        }
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
        /* The GE's parameter-cache allocation for the NGG stage.
         *
         * radv programs GE_PC_ALLOC for every NGG pipeline; this driver has
         * never programmed it for any, so every NGG draw has run on whatever
         * the platform left. The compiler computes it through the same
         * ac_compute_late_alloc() radv uses, now that the domain compile is
         * given the device facts, and publishes it as a linked user-config
         * write. Refused rather than guessed if the compiler did not publish
         * it: an unprogrammed register is at least the platform's value,
         * while a fabricated one is nobody's. */
        if(!input->domain.metadata.linkage_ge_pc_alloc_valid ||
           input->domain.metadata.linkage_ge_pc_alloc.offset!=
               PS5VK_UC_OFFSET(0x030980u)) {
            TESS_CREATE_FAIL("domain-pc-alloc");
            rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
        }
        pair->tess_ring_state[4]=(ps5_agc_register){
            input->domain.metadata.linkage_ge_pc_alloc.offset,
            input->domain.metadata.linkage_ge_pc_alloc.value};
    }
/* DIAGNOSTIC (PS5VK_TESS_LEGACY_DOMAIN / PS5VK_TESS_LEGACY_VS_CONTROL): load the legacy hardware-VS
     * image of the evaluation half, take its VS-block registers with the
     * loaded address in PGM_LO/HI_VS (sh 0x48/0x49), its legacy context
     * state, its stage enables VERBATIM (LS, HS, VS_STAGE_DS, DYNAMIC_HS -
     * no ES, no primitive generator), and its own draw ABI so the user
     * data block matches the program that actually launches. The NGG
     * domain stays loaded and linked for the pixel interpolation; with
     * its stage disabled its registers are latched and unused. */
    if(has_legacy) {
        const PsbcShaderMetadata *lm=&input->domain_legacy.metadata;
        void *legacy_code=(unsigned char *)address+legacy_at;
        memcpy(legacy_code,input->domain_legacy.machine_code,
            input->domain_legacy.machine_code_size);
        const uint64_t legacy_va=(uintptr_t)legacy_code;
        if((legacy_va&255u) || lm->hardware_stage!=PSBC_HW_STAGE_VERTEX ||
           !lm->linkage_valid || lm->shader_register_count>8 ||
           lm->context_register_count>8) {
            TESS_CREATE_FAIL("legacy-domain");
            rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
        }
        unsigned patched_lo=0;
        for(uint32_t i=0;i<lm->shader_register_count;++i) {
            ps5_agc_register r={(uint16_t)lm->shader_registers[i].offset,
                lm->shader_registers[i].value};
            if(r.offset==0x48){r.value=(uint32_t)(legacy_va>>8);++patched_lo;}
            if(r.offset==0x49)r.value=(uint32_t)((legacy_va>>40)&255u);
            pair->legacy_sh[pair->legacy_sh_count++]=r;
        }
        for(uint32_t i=0;i<lm->context_register_count;++i)
            pair->legacy_cx[pair->legacy_cx_count++]=(ps5_agc_register){
                (uint16_t)lm->context_registers[i].offset,
                lm->context_registers[i].value};
        if(patched_lo!=1) {
            TESS_CREATE_FAIL("legacy-pgm");
            rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
        }
        /* The stage enables VERBATIM from the legacy program, appended to
         * its context list so they win over the linked NGG value on every
         * pipeline shape; a tessellation pair also carries them in its
         * launch state. */
        if(pair->legacy_cx_count>=8) {
            TESS_CREATE_FAIL("legacy-cx");
            rc=VK_ERROR_INITIALIZATION_FAILED;goto failed;
        }
        pair->legacy_cx[pair->legacy_cx_count++]=(ps5_agc_register){0x2d5,
            lm->linkage_stages_en.value};
        if(has_tessellation)
            pair->tess_state[0]=(ps5_agc_register){0x2d5,
                lm->linkage_stages_en.value};
        pair->runtime_arguments=input->arguments_legacy;
        if(pair->runtime_arguments.ring_table_valid) {
            pair->runtime_arguments.ring_table_low=pair->tess_ring_table_low;
            pair->runtime_arguments.ring_table_high=pair->tess_ring_table_high;
        }
        pair->legacy_domain=1;
    }
    pair->vertex_quantization=0x2d;pair->ready=1;
    /* Recorded from the compiled pre-raster program rather than re-derived at
     * draw time: the metadata is freed with the program, and the draw path needs
     * the same fact the descriptor profile used to accept the binding. */
    pair->geometry_preraster=(has_tessellation?input->domain.metadata.source_stage:
        input->vertex.metadata.source_stage)==PSBC_STAGE_GEOMETRY;
    rc=p->memory.flush(p->memory.context,p->backing,0,p->allocation_bytes);
    if(rc!=VK_SUCCESS)goto failed;
    *out=p;return VK_SUCCESS;
failed:
    if(p->shared_rings)ps5vk_tess_storage_release(&p->shared_rings);
    else if(p->rings_backing)p->memory.release(p->memory.context,p->rings_backing);
    if(p->backing)p->memory.release(p->memory.context,p->backing);
    free(p);return rc;
}
