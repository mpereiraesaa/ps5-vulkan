import pathlib
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]

class OffchipLeaseTests(unittest.TestCase):
    def test_native_state_is_checked_and_restored_only_after_completion(self):
        source=r'''
#include "native/tess_offchip_lease.h"
#include <assert.h>
struct fake { uint16_t a,b; int sets,gets,fail_set,fail_get,mismatch; };
static int get(void *p,uint16_t *a,uint16_t *b) {
    struct fake *f=p;++f->gets;*a=f->a;*b=f->b;
    return f->fail_get==f->gets?-1:0;
}
static int set(void *p,uint16_t a,uint16_t b) {
    struct fake *f=p;++f->sets;f->a=a;f->b=b;
    if(f->mismatch==f->sets)++f->b;
    return f->fail_set==f->sets?-1:0;
}
int main(void) {
    struct fake f={.a=2,.b=71};
    struct ps5vk_hs_ops ops={&f,get,set};
    struct ps5vk_hs_lease l={0};
    assert(ps5vk_hs_bind(&l,&ops,0));assert(ps5vk_hs_bind(&l,&ops,32769));
    assert(!f.sets && !f.gets);
    assert(!ps5vk_hs_bind(&l,&ops,8388608));assert(f.a==0 && f.b==255);
    assert(ps5vk_hs_bind(&l,&ops,8388608));
    assert(!ps5vk_hs_submitting(&l));assert(ps5vk_hs_restore(&l));
    assert(f.sets==1);assert(!ps5vk_hs_completed(&l));
    assert(!ps5vk_hs_restore(&l));assert(f.a==2 && f.b==71);
    assert(l.state==PS5VK_HS_IDLE);
    for(int failure=0;failure<3;++failure) {
        f=(struct fake){.a=2,.b=71};l=(struct ps5vk_hs_lease){0};
        if(failure==0)f.fail_set=1;
        if(failure==1)f.fail_get=2;
        if(failure==2)f.mismatch=1;
        assert(ps5vk_hs_bind(&l,&ops,8388608));
        assert(l.state==PS5VK_HS_IDLE && f.a==2 && f.b==71 && f.sets==2);
    }
    f=(struct fake){.a=2,.b=71,.fail_get=1};l=(struct ps5vk_hs_lease){0};
    assert(ps5vk_hs_bind(&l,&ops,8388608));assert(!f.sets);
    for(int failure=0;failure<3;++failure) {
        f=(struct fake){.a=2,.b=71};l=(struct ps5vk_hs_lease){0};
        assert(!ps5vk_hs_bind(&l,&ops,8388608));
        if(failure==0)f.fail_set=2;
        if(failure==1)f.fail_get=3;
        if(failure==2)f.mismatch=2;
        assert(ps5vk_hs_restore(&l));assert(l.state==PS5VK_HS_POISONED);
        int calls=f.sets;assert(ps5vk_hs_bind(&l,&ops,8388608));assert(f.sets==calls);
    }
}
'''
        with tempfile.TemporaryDirectory() as d:
            exe=pathlib.Path(d)/'offchip'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                '-I',str(ROOT),'-x','c','-','-o',str(exe)],input=source,
                text=True,check=True,capture_output=True)
            subprocess.run([str(exe)],check=True,capture_output=True)
