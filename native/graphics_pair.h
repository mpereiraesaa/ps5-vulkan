#ifndef PS5VK_GRAPHICS_PAIR_H
#define PS5VK_GRAPHICS_PAIR_H
#include "ps5_shader_header.h"
#include "ps5_agc_registers.h"
#include "shader_relocate.h"
#include "runtime_shader_storage.h"
#include "runtime_draw_abi.h"

struct ps5vk_graphics_pair {
    struct ps5_shader_arena gs, ps;
    struct ps5_agc_linked_cx cx;
    struct ps5_agc_linked_uc uc;
    unsigned ready;
    uint32_t vertex_quantization;
    /* 1 when the pre-raster program is the merged vertex+geometry pair, so the
     * draw path can tell which stage a descriptor binding may name without
     * keeping the compiled metadata alive. The offline library path leaves it
     * zero: its programs are vertex+fragment only. */
    uint32_t geometry_preraster;
    /* 1 when the pipeline carries the tessellation pair: runtime_vertex is
     * then the DOMAIN half's header (the NGG pre-raster program), the hull's
     * two program views live below, and the draw path appends the hull's
     * register banks plus the driver-owned launch state. */
    uint32_t tessellation;
    /* THE merged LS/HS program: on GFX9+ the vertex and control halves are
     * one hardware stage running one image, so there is one runtime shader
     * here, not a pair. */
    struct ps5vk_runtime_shader runtime_hull;
    /* The driver-owned hull launch state, prepared at create from the hull
     * metadata's workgroup layout and the domain's linked stage enables:
     * VGT_SHADER_STAGES_EN (0x2d5) with the LS/HS enables ORed in, and
     * VGT_LS_HS_CONFIG (0x2d6) with the patch count per workgroup and the
     * input/output control-point counts. */
    ps5_agc_register tess_state[3];
    /* The tessellation ring configuration as user-config registers, prepared
     * at create from the rings this pipeline allocates: VGT_TF_RING_SIZE
     * (0x30938), VGT_HS_OFFCHIP_PARAM (0x3093c), VGT_TF_MEMORY_BASE (0x30940)
     * and its high word (0x30984). Written on every patch draw with the rest
     * of the state, so nothing depends on a previous pipeline's rings. */
    ps5_agc_register tess_ring_state[4];
    /* The ring descriptor table's address, split for the user-data bank:
     * the pipeline's ring block starts with the table (sixteen bytes per
     * ring, audited raw buffer SRDs), and the hull's ring-offsets dwords
     * carry its address. */
    uint32_t tess_ring_table_low, tess_ring_table_high;
    /* The hull's user-data dword the compiler assigned to the ring descriptor
     * table, window-relative. The table address is delivered there because the
     * system-block ring_offsets at s0/s1 is not writable for a merged
     * program on this platform. */
    uint32_t tess_ring_table_slot;
    /* The ring block's GPU address; the backing is tracked by the owning
     * native pipeline and released with it. */
    void *tess_rings;
    struct ps5vk_runtime_shader runtime_vertex,runtime_fragment;
    struct ps5vk_runtime_draw_abi runtime_arguments;
};
struct ps5vk_graphics_stage_extent { uint32_t offset, isa_bytes; };
struct ps5vk_graphics_pair_input {
    const void *image;
    size_t image_bytes;
    const struct ps5vk_shader_relocation *relocations;
    size_t relocation_count;
    struct ps5vk_graphics_stage_extent gs, ps;
    const struct ps5_shader_metadata *metadata;
    uint32_t vertex_quantization;
    const ps5_agc_register *interpolators;
    uint32_t interpolator_count;
};
/* Caller owns disjoint writable pair/image storage for the full pipeline life.
 * Pair must initially be zero-initialized, and source image must be disjoint.
 * Native memory must be CPU/GPU identity-mapped. No submission or allocation is
 * performed here. Caller publishes/flushes both after success, before GPU use.
 * A failed preparation is not usable and has no GPU ownership to retire. */
int ps5vk_graphics_pair_prepare(struct ps5vk_graphics_pair *pair,
    void *mapped_image, size_t capacity, const struct ps5vk_graphics_pair_input *input);
#endif
