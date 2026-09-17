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
    /* Banks: the same ten words at the gfx10 per-index strides (6, 2, 2),
     * with bank zero identical to ps5vk_native_viewport and the last bank at
     * PA_CL_VPORT_XSCALE_15 / PA_SC_VPORT_ZMIN_15 / PA_SC_VPORT_SCISSOR_15. */
    ps5_agc_register bank[PS5VK_VIEWPORT_REGISTERS];
    assert(ps5vk_native_viewport_bank(0,&v,&s,&area,bank)==VK_SUCCESS && !memcmp(bank,r,sizeof(bank)));
    v=(VkViewport){100,50,200,100,0.25f,0.75f};s=(VkRect2D){{120,60},{50,40}};
    assert(ps5vk_native_viewport_bank(1,&v,&s,&area,bank)==VK_SUCCESS);
    assert(bank[0].offset==0x115 && value(bank[0])==100 && bank[1].offset==0x116 && value(bank[1])==200);
    assert(bank[2].offset==0x117 && value(bank[2])==50 && bank[3].offset==0x118 && value(bank[3])==100);
    assert(bank[4].offset==0x119 && value(bank[4])==0.5f && bank[5].offset==0x11a && value(bank[5])==0.25f);
    assert(bank[6].offset==0x0b6 && value(bank[6])==0.25f && bank[7].offset==0x0b7 && value(bank[7])==0.75f);
    assert(bank[8].offset==0x096 && bank[8].value==(0x80000000u|120u|(60u<<16)));
    assert(bank[9].offset==0x097 && bank[9].value==(170u|(100u<<16)));
    assert(ps5vk_native_viewport_bank(15,&v,&s,&area,bank)==VK_SUCCESS);
    assert(bank[0].offset==0x169 && bank[5].offset==0x16e && bank[6].offset==0x0d2 &&
        bank[7].offset==0x0d3 && bank[8].offset==0x0b2 && bank[9].offset==0x0b3);
    /* A bank outside the hardware's sixteen is a caller bug, not a wrap. */
    assert(ps5vk_native_viewport_bank(16,&v,&s,&area,bank)!=VK_SUCCESS && !bank[0].offset && !bank[0].value);
    /* Each bank keeps the render-area intersection and the empty-scissor rule. */
    s=(VkRect2D){{3000,10},{10,10}};
    assert(ps5vk_native_viewport_bank(7,&v,&s,&area,bank)==VK_SUCCESS);
    assert(bank[8].offset==0x0a2 && (bank[8].value&0x7fff)==3000u && (bank[9].value&0x7fff)==3000u);
}
