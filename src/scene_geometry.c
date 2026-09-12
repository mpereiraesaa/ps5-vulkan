#include "scene_geometry.h"
#include <float.h>
#include <string.h>
_Static_assert(sizeof(struct ps5vk_scene_vertex)==24,"audited vertex stride");
int ps5vk_scene_sampler_uv(unsigned test,float *u,int *numerator)
{
    static const int offsets[PS5VK_SAMPLER_PROBE_CASES]={-16,-12,-9,-8,-7,-5,-4,-3,-1,0,1,8,16};
    if(test>=PS5VK_SAMPLER_PROBE_CASES || !u || !numerator)return -1;
    *numerator=4096+offsets[test];*u=(float)*numerator/8192.0f;return 0;
}
int ps5vk_scene_probe_triangle(struct ps5vk_scene_vertex *vertices,uint16_t *indices)
{
    if(!vertices || !indices)return -1;
    memset(vertices,0,PS5VK_SCENE_VERTICES*sizeof(*vertices));
    memset(indices,0,PS5VK_SCENE_INDICES*sizeof(*indices));
    const float x[3]={-2,2,0},y[3]={-1.125f,-1.125f,1.125f};
    for(unsigned i=0;i<3;++i) {
        /* World-space mesh inverse-tilted to cancel scene3d's fixed Rx(.35).
         * Actual transformation/projection/rasterization still run on GPU. */
        vertices[i].position[0]=x[i];
        vertices[i].position[1]=.939372713f*y[i];
        vertices[i].position[2]=-.342897807f*y[i];
        indices[i]=(uint16_t)i;
    }
    return 0;
}
unsigned ps5vk_scene_draws(int split, struct ps5vk_scene_draw out[2])
{
    if(!out || (split!=0 && split!=1))return 0;
    out[0]=(struct ps5vk_scene_draw){split?36u:72u,2,-2};
    out[1]=(struct ps5vk_scene_draw){split?36u:0u,38,-2};
    return split?2u:1u;
}
int ps5vk_scene_geometry(struct ps5vk_scene_vertex *vertices,uint16_t *indices,float angle)
{
    if(!vertices || !indices || !(angle>=-FLT_MAX && angle<=FLT_MAX))return -1;
    static const int corners[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
        {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
    static const unsigned faces[6][4]={{0,3,2,1},{4,5,6,7},{0,4,7,3},
        {1,2,6,5},{0,1,5,4},{3,7,6,2}};
    static const float uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
    static const float centers[2][3]={{-.45f,0,0},{.55f,.12f,-.35f}};
    static const float half[2]={.65f,.5f};
    static const unsigned order[6]={0,1,2,0,2,3};
    for(unsigned cube=0;cube<2;++cube)for(unsigned face=0;face<6;++face) {
        unsigned base=cube*24+face*4;
        for(unsigned v=0;v<4;++v) {
            for(unsigned axis=0;axis<3;++axis)
                vertices[base+v].position[axis]=centers[cube][axis]+half[cube]*corners[faces[face][v]][axis];
            vertices[base+v].uv_angle[0]=uv[v][0];vertices[base+v].uv_angle[1]=uv[v][1];
            vertices[base+v].uv_angle[2]=angle;
        }
        for(unsigned i=0;i<6;++i)indices[cube*36+face*6+i]=(uint16_t)(base+order[i]);
    }
    return 0;
}
