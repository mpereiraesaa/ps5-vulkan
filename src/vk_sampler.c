#include "vk_sampler.h"
#include "graphics_limits.h"
static int address_mode(VkSamplerAddressMode mode)
{
    switch(mode) {
    case VK_SAMPLER_ADDRESS_MODE_REPEAT:return 0;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:return 2;
    default:return -1;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice d,const VkSamplerCreateInfo *info,
    const VkAllocationCallbacks *a,VkSampler *out)
{
    if(!out)return VK_ERROR_UNKNOWN;
    *out=VK_NULL_HANDLE;
    if(!d || !info || info->sType!=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO)return VK_ERROR_UNKNOWN;
    /* Initial single-level normalized RGBA profile. Reject rather than clamp
     * unsupported state or silently enable approximate sampling behavior. */
    if(!d->graphics_enabled || info->pNext || info->flags || info->anisotropyEnable ||
        info->compareEnable || info->unnormalizedCoordinates || info->mipLodBias!=0 ||
        info->minLod!=0 || info->maxLod!=0 ||
        (info->mipmapMode!=VK_SAMPLER_MIPMAP_MODE_NEAREST && info->mipmapMode!=VK_SAMPLER_MIPMAP_MODE_LINEAR) ||
        (info->magFilter!=VK_FILTER_NEAREST && info->magFilter!=VK_FILTER_LINEAR) ||
        (info->minFilter!=VK_FILTER_NEAREST && info->minFilter!=VK_FILTER_LINEAR))return VK_ERROR_FEATURE_NOT_PRESENT;
    int u=address_mode(info->addressModeU),v=address_mode(info->addressModeV),w=address_mode(info->addressModeW);
    if(u<0 || v<0 || w<0)return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Defensive handling beyond the advertised valid-usage budget. Do not
     * count failed allocations or release slots while ownership is retained. */
    if(d->sampler_objects>=PS5VK_MAX_SAMPLERS)return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkAllocationCallbacks saved;VkBool32 custom;
    VkSampler s=ps5vk_object_alloc(d->custom_allocator?&d->allocator:NULL,a,sizeof(*s),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,&saved,&custom);
    if(!s)return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->device=d;s->custom_allocator=custom;if(custom)s->allocator=saved;
    /* Public GFX10 S#: CLAMP_X/Y/Z and XY_MAG/MIN_FILTER. Same fields as
     * Xash3D's ps5_gfx1013_build_ssharp, with independent axis/filter inputs. */
    s->words[0]=(uint32_t)u|((uint32_t)v<<3)|((uint32_t)w<<6);
    s->words[2]=((info->magFilter==VK_FILTER_LINEAR?1u:0u)<<20)|
        ((info->minFilter==VK_FILTER_LINEAR?1u:0u)<<22);
    ++d->graphics_objects;++d->sampler_objects;*out=s;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice d,VkSampler s,const VkAllocationCallbacks *a)
{
    (void)a;if(!s)return;
    if(!d || s->device!=d)return;
    if(s->pending || (d->invalidate && !d->invalidate(d,VK_OBJECT_TYPE_SAMPLER,s))) {
        ++d->lifetime_errors;return;
    }
    --d->graphics_objects;
    --d->sampler_objects;
    ps5vk_object_free(s,&s->allocator,s->custom_allocator);
}
