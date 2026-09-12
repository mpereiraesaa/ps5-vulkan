#include "graphics_pair.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int creates, links, failure;
int32_t sceAgcCreateShader(void **out, void *header, void *code)
{
    assert(header && code); ++creates;
    if (failure == creates) return -1;
    *out = failure == 4 ? code : header;
    return 0;
}
int32_t sceAgcLinkShaders(void *cx, void *uc, void *reserved, void *gs, void *ps, uint32_t primitive)
{
    assert(cx && uc && !reserved && gs && ps && primitive == 4); ++links;
    struct ps5_agc_linked_cx *linked=cx;
    for(unsigned i=0;i<32;++i)linked->spi_ps_input_cntl[i]=(ps5_agc_register){0x191u+i,0xfeedu};
    return failure == 3 ? -1 : 0;
}
int main(void)
{
    unsigned char source[512] = {0};
    memcpy(source + 64, "barefoot", 8);
    memcpy(source + 320, "barefoot", 8);
    void *image = aligned_alloc(256, 512); assert(image);
    ps5_agc_register pre[10] = {{0}}, pixel[9] = {{0}};
    struct ps5_shader_metadata metadata = {.pre_raster_cx=pre, .pre_raster_cx_count=10,
        .pixel_cx=pixel, .pixel_cx_count=9};
    struct ps5vk_shader_relocation relocation = {0, 400, 0, 1};
    ps5_agc_register interpolators[32];
    for(unsigned i=0;i<32;++i)interpolators[i]=(ps5_agc_register){0x191u+i,i+1};
    struct ps5vk_graphics_pair_input in = {source, sizeof(source), &relocation, 1,
        {0,64}, {256,64}, &metadata, 0x2d, interpolators, 2};
    struct ps5vk_graphics_pair rejected={0};
    in.vertex_quantization=0;
    assert(ps5vk_graphics_pair_prepare(&rejected,image,512,&in)==-1 && !rejected.ready);
    in.vertex_quantization=0x2d;
    in.interpolator_count=33;
    assert(ps5vk_graphics_pair_prepare(&rejected,image,512,&in)==-1 && !creates && !links);
    in.interpolator_count=0;
    assert(ps5vk_graphics_pair_prepare(&rejected,image,512,&in)==-1 && !creates && !links);
    in.interpolator_count=2;in.interpolators=NULL;
    assert(ps5vk_graphics_pair_prepare(&rejected,image,512,&in)==-1 && !creates && !links);
    in.interpolators=interpolators;interpolators[1].offset=0x191;
    assert(ps5vk_graphics_pair_prepare(&rejected,image,512,&in)==-1 && !creates && !links);
    interpolators[1].offset=0x192;
    for (failure = 0; failure <= 4; ++failure) {
        struct ps5vk_graphics_pair pair = {0}; creates = links = 0;
        int rc = ps5vk_graphics_pair_prepare(&pair, image, 512, &in);
        assert((rc == 0) == (failure == 0));
        assert(pair.ready == (failure == 0));
        if(!failure)assert(pair.vertex_quantization==0x2d);
        assert(links == (failure == 0 || failure == 3));
        if (!failure) {
            assert(!memcmp(pair.cx.spi_ps_input_cntl,interpolators,2*sizeof(*interpolators)));
            assert(pair.cx.spi_ps_input_cntl[2].value==0xfeed);
            uint32_t value; memcpy(&value, image, 4);
            assert(value == (uint32_t)((uintptr_t)image + 400));
            assert(ps5vk_graphics_pair_prepare(&pair, image, 512, &in) == -1);
            assert(creates == 2 && links == 1);
        }
    }
    struct ps5vk_graphics_pair pair = {0}; creates = links = 0;
    failure=0;in.interpolator_count=32;
    assert(ps5vk_graphics_pair_prepare(&pair,image,512,&in)==0);
    assert(!memcmp(pair.cx.spi_ps_input_cntl,interpolators,sizeof(interpolators)));
    memset(&pair,0,sizeof(pair));creates=links=0;
    in.ps.offset = 0;
    assert(ps5vk_graphics_pair_prepare(&pair, image, 512, &in) == -1);
    assert(!creates && !links);
    free(image);
    puts("Graphics pair preparation: host AGC doubles only, no hardware execution");
}
