/*
 * Copyright (C) 2026 Manuel Pereira
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The GFX10.3 clamp, fixed-border, filter and LOD encodings are adapted from
 * blackbearreloaded/ps5-opengl, src/gallium/ps5/ps5_screen.c at
 * 7f9bfabdddb187a11e4401058eba8c9e55194d0a.
 */
#include "vk_sampler.h"
#include "graphics_limits.h"
#include <float.h>
static int address_mode(VkSamplerAddressMode mode)
{
    switch(mode) {
    case VK_SAMPLER_ADDRESS_MODE_REPEAT:return 0;
    case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:return 1;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:return 2;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:return 6;
    default:return -1;
    }
}
static int border_color(VkBorderColor color)
{
    switch(color) {
    case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:
    case VK_BORDER_COLOR_INT_TRANSPARENT_BLACK:return 0;
    case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
    case VK_BORDER_COLOR_INT_OPAQUE_BLACK:return 1;
    case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
    case VK_BORDER_COLOR_INT_OPAQUE_WHITE:return 2;
    default:return -1;
    }
}
static uint32_t unsigned_lod(float value)
{
    if(value<=0)return 0;
    if(value>=15)return 15u<<8;
    return (uint32_t)(value*256.0f);
}
static uint32_t signed_lod_bias(float value)
{
    /* GFX10 S# LOD_BIAS is signed 8.8 in the low fourteen bits. Creation has
     * already bounded the public Vulkan interval to [-2, 2]. */
    return (uint32_t)(int32_t)(value*256.0f)&0x3fffu;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice d,const VkSamplerCreateInfo *info,
    const VkAllocationCallbacks *a,VkSampler *out)
{
    if(!out)return VK_ERROR_UNKNOWN;
    *out=VK_NULL_HANDLE;
    if(!d || !info || info->sType!=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO)return VK_ERROR_UNKNOWN;
    const VkSamplerCreateFlags unsupported_flags = info->flags;
    const VkBool32 compare_allowed = info->compareEnable &&
        info->compareOp >= VK_COMPARE_OP_NEVER &&
        info->compareOp <= VK_COMPARE_OP_ALWAYS;
    /* Reject rather than clamp unsupported state or silently enable
     * approximate sampling behavior. */
    if(!d->graphics_enabled || info->pNext || unsupported_flags || info->anisotropyEnable ||
        (info->compareEnable && (!compare_allowed || info->minLod < 0)) ||
        info->unnormalizedCoordinates ||
        !(info->mipLodBias>=-(float)PS5VK_MAX_SAMPLER_LOD_BIAS &&
          info->mipLodBias<=(float)PS5VK_MAX_SAMPLER_LOD_BIAS) ||
        !(info->minLod>=-FLT_MAX && info->minLod<=FLT_MAX &&
          info->maxLod>=info->minLod && info->maxLod<=FLT_MAX) ||
        (info->mipmapMode!=VK_SAMPLER_MIPMAP_MODE_NEAREST && info->mipmapMode!=VK_SAMPLER_MIPMAP_MODE_LINEAR) ||
        (info->magFilter!=VK_FILTER_NEAREST && info->magFilter!=VK_FILTER_LINEAR) ||
        (info->minFilter!=VK_FILTER_NEAREST && info->minFilter!=VK_FILTER_LINEAR))return VK_ERROR_FEATURE_NOT_PRESENT;
    int u=address_mode(info->addressModeU),v=address_mode(info->addressModeV),w=address_mode(info->addressModeW);
    int border=border_color(info->borderColor);
    if(u<0 || v<0 || w<0 || border<0)return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Defensive handling beyond the advertised valid-usage budget. Do not
     * count failed allocations or release slots while ownership is retained. */
    if(d->sampler_objects>=PS5VK_MAX_SAMPLERS)return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkAllocationCallbacks saved;VkBool32 custom;
    VkSampler s=ps5vk_object_alloc(d->custom_allocator?&d->allocator:NULL,a,sizeof(*s),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,&saved,&custom);
    if(!s)return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->device=d;s->custom_allocator=custom;s->compare_enable=info->compareEnable;
    if(custom)s->allocator=saved;
    /* Public GFX10 S#: CLAMP_X/Y/Z and XY_MAG/MIN_FILTER. Same fields as
     * Xash3D's ps5_gfx1013_build_ssharp, with independent axis/filter inputs. */
    s->words[0]=(uint32_t)u|((uint32_t)v<<3)|((uint32_t)w<<6);
    if(info->compareEnable)
        s->words[0]|=((uint32_t)info->compareOp&7u)<<12;
    s->words[1]=unsigned_lod(info->minLod)|(unsigned_lod(info->maxLod)<<12);
    s->words[2]=signed_lod_bias(info->mipLodBias)|
        ((info->magFilter==VK_FILTER_LINEAR?1u:0u)<<20)|
        ((info->minFilter==VK_FILTER_LINEAR?1u:0u)<<22)|
        ((info->mipmapMode==VK_SAMPLER_MIPMAP_MODE_LINEAR?2u:1u)<<26);
    s->words[3]=(uint32_t)border<<30;
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
