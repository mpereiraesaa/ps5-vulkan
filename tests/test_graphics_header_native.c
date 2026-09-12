/* Compile against a newly generated owned-shader header, not a fixture. */
#include "graphics_metadata.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct ps5_shader_arena gs, ps;
    /* The triangle control's original 304/52-byte stages plus 48-byte trailers. */
    assert(ps5_shader_header_build(&gs, PS5_SHADER_PRE_RASTER, 352,
                                   &ps5vk_graphics_metadata) == 0);
    assert(ps5_shader_header_build(&ps, PS5_SHADER_PIXEL, 100,
                                   &ps5vk_graphics_metadata) == 0);
    assert(ps5_shader_header_validate(&gs, PS5_SHADER_PRE_RASTER, 352,
                                      &ps5vk_graphics_metadata) == 0);
    assert(ps5_shader_header_validate(&ps, PS5_SHADER_PIXEL, 100,
                                      &ps5vk_graphics_metadata) == 0);
    assert(gs.sh[2].value == ps5vk_graphics_metadata.gs_rsrc1);
    assert(ps.sh[4].value == ps5vk_graphics_metadata.ps_rsrc1);
    assert(gs.header.code == NULL && ps.header.code == NULL);
    puts("Generated graphics metadata accepted by reusable header builder; host only");
    return 0;
}
