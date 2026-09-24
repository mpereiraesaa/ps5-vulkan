#include "vk_sampler.h"
#include "graphics_limits.h"
#include <assert.h>
#include <math.h>
static VkBool32 allow=VK_TRUE;
static unsigned allocation_attempts;
static void *VKAPI_PTR fail_alloc(void *u,size_t n,size_t a,VkSystemAllocationScope s)
{(void)u;(void)n;(void)a;(void)s;++allocation_attempts;return NULL;}
static void *VKAPI_PTR fail_realloc(void *u,void *p,size_t n,size_t a,VkSystemAllocationScope s)
{(void)u;(void)p;(void)n;(void)a;(void)s;return NULL;}
static void VKAPI_PTR no_free(void *u,void *p){(void)u;(void)p;assert(0);}
static VkBool32 invalidate(VkDevice d,VkObjectType type,const void *p)
{(void)d;assert(type==VK_OBJECT_TYPE_SAMPLER && p);return allow;}
int main(void)
{
    struct VkDevice_T d={.graphics_enabled=VK_TRUE,.invalidate=invalidate};
    VkSamplerCreateInfo info={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler sampler;
    VkAllocationCallbacks failed_allocator={.pfnAllocation=fail_alloc,.pfnReallocation=fail_realloc,.pfnFree=no_free};
    assert(vkCreateSampler(&d,&info,&failed_allocator,&sampler)==VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(allocation_attempts==1 && !sampler && !d.sampler_objects && !d.graphics_objects);
    assert(vkCreateSampler(&d,&info,NULL,&sampler)==VK_SUCCESS);
    assert(d.sampler_objects==1 && d.graphics_objects==1 && !sampler->words[0] && !sampler->words[1] &&
        sampler->words[2]==(1u<<26) && !sampler->words[3]);
    sampler->pending=1;vkDestroySampler(&d,sampler,NULL);assert(d.graphics_objects==1);
    sampler->pending=0;allow=VK_FALSE;vkDestroySampler(&d,sampler,NULL);assert(d.graphics_objects==1);
    assert(d.sampler_objects==1);
    allow=VK_TRUE;vkDestroySampler(&d,sampler,NULL);assert(!d.sampler_objects && !d.graphics_objects && d.lifetime_errors==2);
    info.magFilter=VK_FILTER_LINEAR;info.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    assert(vkCreateSampler(&d,&info,NULL,&sampler)==VK_SUCCESS);
    assert(sampler->words[0]==16 && sampler->words[2]==((1u<<20)|(1u<<26)));
    vkDestroySampler(&d,sampler,NULL);
    info.minLod=0.5f;info.maxLod=17.0f;
    assert(vkCreateSampler(&d,&info,NULL,&sampler)==VK_SUCCESS);
    assert(sampler->words[1]==(128u|(15u<<20)));vkDestroySampler(&d,sampler,NULL);
    info.minLod=0;info.maxLod=0;
    info.magFilter=VK_FILTER_NEAREST;info.minFilter=VK_FILTER_LINEAR;
    info.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    assert(vkCreateSampler(&d,&info,NULL,&sampler)==VK_SUCCESS);
    assert(sampler->words[0]==146 && sampler->words[2]==((1u<<22)|(1u<<26)));
    vkDestroySampler(&d,sampler,NULL);
    info.addressModeU=VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    info.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.borderColor=VK_BORDER_COLOR_INT_OPAQUE_WHITE;
    assert(vkCreateSampler(&d,&info,NULL,&sampler)==VK_SUCCESS);
    assert(sampler->words[0]==49 && sampler->words[2]==((1u<<22)|(2u<<26)) &&
        sampler->words[3]==(2u<<30));
    vkDestroySampler(&d,sampler,NULL);
    info.mipLodBias=2.0f;
    assert(vkCreateSampler(&d,&info,NULL,&sampler)==VK_SUCCESS);
    assert(sampler->words[2]==(512u|(1u<<22)|(2u<<26)));
    vkDestroySampler(&d,sampler,NULL);
    info.mipLodBias=-2.0f;
    assert(vkCreateSampler(&d,&info,NULL,&sampler)==VK_SUCCESS);
    assert(sampler->words[2]==(0x3e00u|(1u<<22)|(2u<<26)));
    vkDestroySampler(&d,sampler,NULL);info.mipLodBias=0;
#define BAD(field,value) do { VkSamplerCreateInfo bad=info;bad.field=value;sampler=(VkSampler)(uintptr_t)1; \
    assert(vkCreateSampler(&d,&bad,NULL,&sampler)!=VK_SUCCESS && !sampler && !d.graphics_objects); } while(0)
    BAD(pNext,&info);BAD(flags,1);BAD(anisotropyEnable,VK_TRUE);BAD(compareEnable,VK_TRUE);
    BAD(flags,VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT);
    BAD(unnormalizedCoordinates,VK_TRUE);BAD(mipLodBias,NAN);BAD(mipLodBias,2.01f);
    BAD(mipLodBias,-2.01f);BAD(minLod,NAN);BAD(maxLod,INFINITY);
    BAD(magFilter,VK_FILTER_CUBIC_EXT);BAD(minFilter,VK_FILTER_CUBIC_EXT);
    BAD(addressModeW,VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE);
    BAD(borderColor,(VkBorderColor)99);
    BAD(mipmapMode,(VkSamplerMipmapMode)99);BAD(maxLod,-1.0f);
    BAD(minLod,1.0f); /* maxLod remains zero. */
    info.minLod=-1000.0f;info.maxLod=1000.0f;
    assert(vkCreateSampler(&d,&info,NULL,&sampler)==VK_SUCCESS);
    assert(sampler->words[1]==(15u<<20));
    vkDestroySampler(&d,sampler,NULL);
    info.minLod=0.0f;info.maxLod=0.0f;
    VkSampler slots[PS5VK_MAX_SAMPLERS];
    for(unsigned i=0;i<PS5VK_MAX_SAMPLERS;++i)
        assert(vkCreateSampler(&d,&info,NULL,&slots[i])==VK_SUCCESS);
    assert(d.sampler_objects==PS5VK_MAX_SAMPLERS);
    assert(vkCreateSampler(&d,&info,&failed_allocator,&sampler)==VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(allocation_attempts==1 && !sampler); /* Budget rejects before allocation. */
    sampler=(VkSampler)(uintptr_t)1;
    assert(vkCreateSampler(&d,&info,NULL,&sampler)==VK_ERROR_OUT_OF_HOST_MEMORY && !sampler);
    slots[0]->pending=1;
    vkDestroySampler(&d,slots[0],NULL);
    assert(d.sampler_objects==PS5VK_MAX_SAMPLERS);
    assert(vkCreateSampler(&d,&info,NULL,&sampler)==VK_ERROR_OUT_OF_HOST_MEMORY && !sampler);
    slots[0]->pending=0;vkDestroySampler(&d,slots[0],NULL);
    assert(d.sampler_objects==PS5VK_MAX_SAMPLERS-1);
    assert(vkCreateSampler(&d,&info,NULL,&slots[0])==VK_SUCCESS);
    for(unsigned i=0;i<PS5VK_MAX_SAMPLERS;++i)vkDestroySampler(&d,slots[i],NULL);
    assert(!d.sampler_objects && !d.graphics_objects);
    d.graphics_enabled=VK_FALSE;assert(vkCreateSampler(&d,&info,NULL,&sampler)!=VK_SUCCESS);
    vkDestroySampler(&d,VK_NULL_HANDLE,NULL);
}
