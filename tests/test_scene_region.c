#include "scene_region.h"
#include <assert.h>
#include <stdlib.h>
int main(void)
{
    size_t n=15*8*16384;
    uint32_t *p=calloc(n,sizeof(*p));assert(p);
    size_t offset=(4*15+7)*16384;
    for(size_t i=0;i<16384;++i)p[offset+i]=i%2 ? 0xffff0000 : 0xff00ff00;
    struct ps5vk_scene_region r;
    assert(!ps5vk_scene_region_scan(p,n,7,4,&r));
    assert(r.red==8192 && r.green==8192 && !r.black && !r.blue && !r.unexpected);
    p[offset]=0xff000000;p[offset+1]=0xff0000ff;
    assert(!ps5vk_scene_region_scan(p,n,7,4,&r));
    assert(r.black==1 && r.blue==1 && r.red==8191 && r.green==8191);
    assert(ps5vk_scene_region_scan(p,offset+16383,7,4,&r)==-1);
    assert(ps5vk_scene_region_scan(p,n,15,4,&r)==-1);
    assert(ps5vk_scene_region_scan(p,n,7,8,&r)==-1);
    assert(ps5vk_scene_region_scan(NULL,n,0,0,&r)==-1);
    free(p);
}
