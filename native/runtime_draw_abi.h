#ifndef PS5VK_RUNTIME_DRAW_ABI_H
#define PS5VK_RUNTIME_DRAW_ABI_H
#include <stdint.h>

/* Descriptor-free runtime profile. UINT32_MAX denotes an unused argument.
 * Counts and slots originate from compiler metadata, not pipeline filenames. */
struct ps5vk_runtime_draw_abi {
    uint32_t enabled;
    uint32_t vertex_count, fragment_count;
    uint32_t base_vertex_slot, start_instance_slot, lds_slot, lds_value;
};

static inline int ps5vk_runtime_draw_values(const struct ps5vk_runtime_draw_abi *a,
    uint32_t base_vertex, uint32_t instance, uint32_t vertex[16], uint32_t pixel[16])
{
    if (!a || a->enabled!=1 || !a->vertex_count || a->vertex_count>16 ||
        a->fragment_count>16 || a->lds_slot>=a->vertex_count || a->lds_value>UINT16_MAX)
        return -1;
    uint32_t slots[3]={a->base_vertex_slot,a->start_instance_slot,a->lds_slot};
    for(unsigned i=0;i<3;++i) {
        if(slots[i]==UINT32_MAX)continue;
        if(slots[i]>=a->vertex_count)return -1;
        for(unsigned j=0;j<i;++j)if(slots[i]==slots[j])return -1;
    }
    for(unsigned i=0;i<16;++i)vertex[i]=pixel[i]=0;
    if(a->base_vertex_slot!=UINT32_MAX)vertex[a->base_vertex_slot]=base_vertex;
    if(a->start_instance_slot!=UINT32_MAX)vertex[a->start_instance_slot]=instance;
    vertex[a->lds_slot]=a->lds_value;
    return 0;
}
#endif
