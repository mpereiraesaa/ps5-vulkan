#include "viewport_ps5.h"
#include <assert.h>
#include <math.h>
#include <string.h>
static float value(ps5_agc_register r) { float f; memcpy(&f, &r.value, sizeof(f)); return f; }
int main(void)
{
    VkViewport v = {20, 30, 100, 200, 0.8f, 0.2f};
    VkRect2D s = {{10, 40}, {100, 200}}, area = {{30, 0}, {100, 100}};
    ps5_agc_register r[PS5VK_VIEWPORT_REGISTERS];
    assert(ps5vk_native_viewport(&v, &s, &area, r) == VK_SUCCESS);
    assert(value(r[0]) == 50 && value(r[1]) == 70 && value(r[2]) == 100 && value(r[3]) == 130);
    assert(value(r[4]) == v.maxDepth - v.minDepth && value(r[5]) == 0.8f);
    assert(value(r[6]) == 0.2f && value(r[7]) == 0.8f);
    assert(r[8].value == (0x80000000u | 30 | (40u << 16)) && r[9].value == (110 | (100u << 16)));
    s.offset.x = 200;
    assert(ps5vk_native_viewport(&v, &s, &area, r) == VK_SUCCESS);
    assert((r[8].value & 0x7fff) == (r[9].value & 0x7fff));
    v.height = -200;
    assert(ps5vk_native_viewport(&v, &s, &area, r) != VK_SUCCESS && !r[0].value);
    v.height = NAN;
    assert(ps5vk_native_viewport(&v, &s, &area, r) != VK_SUCCESS);
    v.height = 200; s.extent.width = UINT32_MAX;
    assert(ps5vk_native_viewport(&v, &s, &area, r) != VK_SUCCESS);
    v=(VkViewport){0,0,1920,1080,0,1};
    s=(VkRect2D){{768,384},{128,128}};area=(VkRect2D){{0,0},{1920,1080}};
    assert(ps5vk_native_viewport(&v,&s,&area,r)==VK_SUCCESS);
    assert(value(r[0])==960 && value(r[1])==960);
    assert(value(r[2])==540 && value(r[3])==540);
    assert(r[8].offset==0x094 && r[8].value==(0x80000000u|768u|(384u<<16)));
    assert(r[9].offset==0x095 && r[9].value==(896u|(512u<<16)));
}
