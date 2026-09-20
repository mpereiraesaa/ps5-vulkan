#ifndef PS5VK_TESS_OFFCHIP_LEASE_H
#define PS5VK_TESS_OFFCHIP_LEASE_H
#include <stdint.h>
/* Exclusive console/queue ownership is external. Native 32KiB-block profile;
 * never restore while submitted GPU work may still reference the allocation. */
enum ps5vk_hs_state { PS5VK_HS_IDLE,PS5VK_HS_BOUND,PS5VK_HS_INFLIGHT,PS5VK_HS_POISONED };
struct ps5vk_hs_ops {
    void *context;
    int (*get)(void *,uint16_t *,uint16_t *);
    int (*set)(void *,uint16_t,uint16_t);
};
struct ps5vk_hs_lease {
    enum ps5vk_hs_state state;
    uint16_t previous_first,previous_second;
    struct ps5vk_hs_ops ops;
};
static inline int ps5vk_hs_restore(struct ps5vk_hs_lease *l)
{
    if(!l || l->state!=PS5VK_HS_BOUND)return -1;
    l->state=PS5VK_HS_POISONED;
    int set=l->ops.set(l->ops.context,l->previous_first,l->previous_second);
    uint16_t a=0,b=0;
    int get=l->ops.get(l->ops.context,&a,&b);
    if(set || get || a!=l->previous_first || b!=l->previous_second)return -1;
    l->state=PS5VK_HS_IDLE;return 0;
}
static inline int ps5vk_hs_bind(struct ps5vk_hs_lease *l,
    const struct ps5vk_hs_ops *ops,uint32_t allocation_bytes)
{
    if(!l || l->state!=PS5VK_HS_IDLE || !ops || !ops->get || !ops->set ||
       !allocation_bytes || allocation_bytes%32768u ||
       allocation_bytes/32768u>65536u)return -1;
    uint16_t a=0,b=0;
    if(ops->get(ops->context,&a,&b))return -1;
    l->ops=*ops;l->previous_first=a;l->previous_second=b;
    l->state=PS5VK_HS_BOUND;
    uint16_t target=(uint16_t)(allocation_bytes/32768u-1u);
    int set=ops->set(ops->context,0,target);
    int get=ops->get(ops->context,&a,&b);
    if(set || get || a!=0 || b!=target) {
        (void)ps5vk_hs_restore(l);return -1;
    }
    return 0;
}
static inline int ps5vk_hs_submitting(struct ps5vk_hs_lease *l)
{
    if(!l || l->state!=PS5VK_HS_BOUND)return -1;
    l->state=PS5VK_HS_INFLIGHT;return 0;
}
static inline int ps5vk_hs_completed(struct ps5vk_hs_lease *l)
{
    if(!l || l->state!=PS5VK_HS_INFLIGHT)return -1;
    l->state=PS5VK_HS_BOUND;return 0;
}
#endif
