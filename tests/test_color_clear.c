#include "color_clear.h"
#include <assert.h>
#include <math.h>
int main(void)
{
    uint32_t value=123;
    assert(ps5vk_color_clear_bgra8((float[]){0,0,0,1},&value) && value==0xff000000);
    assert(ps5vk_color_clear_bgra8((float[]){1,0,.5f,1},&value) && value==0xffff0080);
    assert(ps5vk_color_clear_bgra8((float[]){0,1,1,0},&value) && value==0x0000ffff);
    uint32_t saved=value;
    assert(!ps5vk_color_clear_bgra8((float[]){1,0,0,NAN},&value) && value==saved);
    assert(!ps5vk_color_clear_bgra8((float[]){INFINITY,0,0,1},&value) && value==saved);
    assert(!ps5vk_color_clear_bgra8((float[]){-1,0,0,1},&value) && value==saved);
    assert(!ps5vk_color_clear_bgra8((float[]){2,0,0,1},&value) && value==saved);
    assert(!ps5vk_color_clear_bgra8(0,&value));
    assert(!ps5vk_color_clear_bgra8((float[]){0,0,0,1},0));
    assert(ps5vk_color_clear_rgba8((float[]){1,0,.5f,1},&value) && value==0xff8000ff);
    assert(!ps5vk_color_clear_rgba8((float[]){0,0,NAN,1},&value) && value==0xff8000ff);
    assert(ps5vk_color_clear_rgba8_uint((uint32_t[]){0,128,255,300},&value) &&
           value==UINT32_C(0x2cff8000));
    assert(ps5vk_color_clear_rgba8_sint((int32_t[]){-128,-1,1,127},&value) &&
           value==UINT32_C(0x7f01ff80));
    assert(!ps5vk_color_clear_rgba8_sint((int32_t[]){-129,-1,1,127},&value) &&
           value==UINT32_C(0x7f01ff80));
}
