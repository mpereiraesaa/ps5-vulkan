#include "scene_geometry.h"
#include <assert.h>
#include <math.h>
#include <string.h>
int main(void)
{
    struct ps5vk_scene_vertex a[PS5VK_SCENE_VERTICES],b[PS5VK_SCENE_VERTICES];
    uint16_t indices[PS5VK_SCENE_INDICES],other[PS5VK_SCENE_INDICES];
    assert(!ps5vk_scene_geometry(a,indices,0));assert(!ps5vk_scene_geometry(b,other,.75f));
    assert(!memcmp(indices,other,sizeof(indices)));
    for(int split=0;split<=1;++split) {
        struct ps5vk_scene_draw draws[2];
        unsigned count=ps5vk_scene_draws(split,draws),cursor=0;
        assert(count==(unsigned)split+1);
        for(unsigned j=0;j<count;++j)for(unsigned k=0;k<draws[j].count;++k) {
            unsigned index=draws[j].first_index-2+k;
            assert(index==cursor++);
            /* Uploaded indices add 3; signed base vertex stays -2. */
            assert((int)indices[index]+3+draws[j].base_vertex==(int)indices[index]+1);
        }
        assert(cursor==PS5VK_SCENE_INDICES);
    }
    struct ps5vk_scene_draw invalid[2];
    assert(!ps5vk_scene_draws(2,invalid));assert(!ps5vk_scene_draws(0,NULL));
    for(unsigned i=0;i<PS5VK_SCENE_VERTICES;++i) {
        assert(!memcmp(a[i].position,b[i].position,sizeof(a[i].position)));
        assert(b[i].uv_angle[2]==.75f);
        for(unsigned k=0;k<2;++k)assert(a[i].uv_angle[k]>=0 && a[i].uv_angle[k]<=1);
    }
    const float centers[2][3]={{-.45f,0,0},{.55f,.12f,-.35f}};
    for(unsigned i=0;i<PS5VK_SCENE_INDICES;i+=3) {
        for(unsigned j=0;j<3;++j)assert(indices[i+j]<PS5VK_SCENE_VERTICES);
        const float *p=a[indices[i]].position,*q=a[indices[i+1]].position,*r=a[indices[i+2]].position;
        float u[3],v[3];for(unsigned k=0;k<3;++k){u[k]=q[k]-p[k];v[k]=r[k]-p[k];}
        float n[3]={u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
        float dot=0;for(unsigned k=0;k<3;++k)dot+=n[k]*(p[k]-centers[i/36][k]);
        assert(dot>0); /* Nondegenerate outward winding on every face. */
    }
    assert(ps5vk_scene_geometry(b,other,NAN));
    for(unsigned i=0;i<PS5VK_SCENE_VERTICES;++i)assert(b[i].uv_angle[2]==.75f);
    assert(!ps5vk_scene_probe_triangle(b,other));
    const float expected[3][2]={{384,216},{1536,216},{960,864}};
    for(unsigned i=0;i<3;++i) {
        float y=.939372713f*b[i].position[1]-.342897807f*b[i].position[2];
        float z=.342897807f*b[i].position[1]+.939372713f*b[i].position[2];
        float w=4-z;
        float sx=960+960*b[i].position[0]*1.2f/w,sy=540+540*y*2.133333333f/w;
        assert(fabsf(sx-expected[i][0])<.001f && fabsf(sy-expected[i][1])<.001f);
        assert(other[i]==i && b[i].uv_angle[0]==0 && b[i].uv_angle[1]==0 && b[i].uv_angle[2]==0);
    }
    assert(!ps5vk_scene_probe_triangle_uv(b));
    const float expected_uv[3][2]={{0,0},{1,0},{.5f,1}};
    for(unsigned i=0;i<3;++i) {
        assert(b[i].uv_angle[0]==expected_uv[i][0]);
        assert(b[i].uv_angle[1]==expected_uv[i][1]);
        assert(b[i].uv_angle[2]==0);
    }
    assert(ps5vk_scene_probe_triangle_uv(NULL));
    for(unsigned probe=0;probe<=13;++probe)
        assert(ps5vk_scene_full_frame_probe(probe)==(probe>=4 && probe<=12));
    for(unsigned i=3;i<PS5VK_SCENE_VERTICES;++i)for(unsigned j=0;j<3;++j)
        assert(b[i].position[j]==0 && b[i].uv_angle[j]==0);
    for(unsigned i=3;i<PS5VK_SCENE_INDICES;++i)assert(other[i]==0);
    assert(ps5vk_scene_probe_triangle(NULL,other));assert(ps5vk_scene_probe_triangle(b,NULL));
    int previous=0;
    for(unsigned i=0;i<PS5VK_SAMPLER_PROBE_CASES;++i) {
        float u;int numerator;
        assert(!ps5vk_scene_sampler_uv(i,&u,&numerator));
        assert(numerator>previous && u*8192==numerator);previous=numerator;
    }
    float u=7;int numerator=8;
    assert(ps5vk_scene_sampler_uv(PS5VK_SAMPLER_PROBE_CASES,&u,&numerator));
    assert(u==7 && numerator==8);
    assert(ps5vk_scene_sampler_uv(0,NULL,&numerator));assert(ps5vk_scene_sampler_uv(0,&u,NULL));
}
