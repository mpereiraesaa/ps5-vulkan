#include "graphics_pair.h"
#include <string.h>

static int extent_valid(struct ps5vk_graphics_stage_extent s, size_t bytes)
{
    return !(s.offset & 255u) && s.isa_bytes && !(s.isa_bytes & 3u) &&
        s.offset <= bytes && s.isa_bytes <= bytes - s.offset &&
        bytes - s.offset - s.isa_bytes >= 48u;
}

int ps5vk_graphics_pair_prepare(struct ps5vk_graphics_pair *pair,
    void *mapped_image, size_t capacity, const struct ps5vk_graphics_pair_input *in)
{
    if (!pair) return -1;
    /* Re-preparing a live pair would overwrite self-relative shader storage. */
    if (pair->ready) return -1;
    if (!in || !in->image || !in->metadata || in->vertex_quantization != 0x2d || !mapped_image ||
        ((uintptr_t)mapped_image & 255u) || !in->image_bytes ||
        in->image_bytes > capacity || in->image_bytes > 16u * 1024u * 1024u ||
        !extent_valid(in->gs, in->image_bytes) || !extent_valid(in->ps, in->image_bytes)) return -1;
    if (!in->interpolators || !in->interpolator_count || in->interpolator_count>32) return -1;
    for (uint32_t i=0;i<in->interpolator_count;++i)
        if(in->interpolators[i].offset!=0x191u+i)return -1;
    uint64_t gs_end = (uint64_t)in->gs.offset + in->gs.isa_bytes + 48u;
    uint64_t ps_end = (uint64_t)in->ps.offset + in->ps.isa_bytes + 48u;
    if (in->gs.offset < ps_end && in->ps.offset < gs_end) return -1;
    const unsigned char *source = in->image;
    if (memcmp(source + in->gs.offset + in->gs.isa_bytes, "barefoot", 8) ||
        memcmp(source + in->ps.offset + in->ps.isa_bytes, "barefoot", 8)) return -1;
    memset(pair, 0, sizeof(*pair));
    if (ps5_shader_header_build(&pair->gs, PS5_SHADER_PRE_RASTER, in->gs.isa_bytes + 48u,
                                in->metadata) ||
        ps5_shader_header_build(&pair->ps, PS5_SHADER_PIXEL, in->ps.isa_bytes + 48u,
                                in->metadata)) return -2;
    memcpy(mapped_image, source, in->image_bytes);
    if (ps5vk_shader_relocate(mapped_image, in->image_bytes, (uintptr_t)mapped_image,
                              in->relocations, in->relocation_count)) return -3;
    void *gs = NULL, *ps = NULL;
    if (sceAgcCreateShader(&gs, &pair->gs, (unsigned char *)mapped_image + in->gs.offset) ||
        gs != &pair->gs) return -4;
    if (sceAgcCreateShader(&ps, &pair->ps, (unsigned char *)mapped_image + in->ps.offset) ||
        ps != &pair->ps) return -5;
    if (sceAgcLinkShaders(&pair->cx, &pair->uc, NULL, gs, ps, 4u)) return -6;
    /* LLPC specifies interpolation modes and export offsets. Linking supplies
     * the native structural state, but must not silently replace that compiler
     * contract with default smooth/identity interpolation. */
    memcpy(pair->cx.spi_ps_input_cntl,in->interpolators,
           in->interpolator_count*sizeof(*in->interpolators));
    pair->vertex_quantization = in->vertex_quantization;
    pair->ready = 1;
    return 0;
}
