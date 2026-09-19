#include "tess_ring_lease.h"
#include <assert.h>
#include <stddef.h>
struct mock {
    uint64_t address;
    uint32_t size;
    unsigned gets, sets, fail_get, fail_set, mismatch_get;
};
static int get(void *p,uint64_t *address,uint32_t *size)
{
    struct mock *m=p;
    ++m->gets;
    if(m->gets==m->fail_get)return -1;
    *address=m->address;*size=m->size;
    if(m->gets==m->mismatch_get)*address+=256;
    return 0;
}
static int set(void *p,uint64_t address,uint32_t size)
{
    struct mock *m=p;
    ++m->sets;m->address=address;m->size=size;
    return m->sets==m->fail_set ? -1 : 0;
}
static void failure(unsigned get_failure,unsigned set_failure,unsigned mismatch,
                    enum ps5vk_tess_ring_state expected)
{
    struct mock m={.address=0x1000,.size=128,.fail_get=get_failure,
        .fail_set=set_failure,.mismatch_get=mismatch};
    struct ps5vk_tess_ring_ops ops={&m,get,set};
    struct ps5vk_tess_ring_lease lease={0};
    int rc=ps5vk_tess_ring_bind(&lease,&ops,0x2000,256);
    if(!rc)assert(ps5vk_tess_ring_restore(&lease)==-1);
    assert(lease.state==expected);
    if(get_failure==1)assert(m.sets==0);
    if(expected==PS5VK_TF_POISONED) {
        unsigned calls=m.gets+m.sets;
        assert(ps5vk_tess_ring_restore(&lease)==-1);
        assert(ps5vk_tess_ring_bind(&lease,&ops,0x3000,256)==-1);
        assert(m.gets+m.sets==calls);
    }
}
int main(void)
{
    struct mock m={.address=0x1000,.size=128};
    struct ps5vk_tess_ring_ops ops={&m,get,set};
    struct ps5vk_tess_ring_lease lease={0};
    assert(ps5vk_tess_ring_bind(&lease,&ops,0x2001,256)==-1);
    assert(ps5vk_tess_ring_bind(&lease,&ops,0x2000,3)==-1);
    assert(m.gets==0 && m.sets==0);
    assert(!ps5vk_tess_ring_bind(&lease,&ops,0x2000,256));
    assert(ps5vk_tess_ring_completed(&lease)==-1);
    assert(ps5vk_tess_ring_bind(&lease,&ops,0x3000,256)==-1);
    assert(!ps5vk_tess_ring_submitting(&lease));
    unsigned calls=m.gets+m.sets;
    assert(ps5vk_tess_ring_restore(&lease)==-1);
    assert(ps5vk_tess_ring_submitting(&lease)==-1);
    assert(m.gets+m.sets==calls);
    assert(!ps5vk_tess_ring_completed(&lease));
    assert(!ps5vk_tess_ring_restore(&lease));
    assert(lease.state==PS5VK_TF_IDLE && m.address==0x1000 && m.size==128);
    assert(ps5vk_tess_ring_restore(&lease)==-1);
    failure(1,0,0,PS5VK_TF_IDLE); /* snapshot failure: no setter */
    failure(0,1,0,PS5VK_TF_IDLE); /* setter mutates then fails: restore */
    failure(2,0,0,PS5VK_TF_IDLE); /* bind verification failure */
    failure(0,0,2,PS5VK_TF_IDLE); /* bind verification mismatch */
    failure(0,2,0,PS5VK_TF_POISONED); /* restore setter uncertain */
    failure(3,0,0,PS5VK_TF_POISONED); /* restore getter failure */
    failure(0,0,3,PS5VK_TF_POISONED); /* restore mismatch */
    failure(2,2,0,PS5VK_TF_POISONED); /* rollback itself fails */
    return 0;
}
