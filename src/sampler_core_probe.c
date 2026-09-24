/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sampler_core_probe.h"
int ps5vk_sampler_core_color_near(uint32_t actual,uint32_t expected,unsigned tolerance)
{
    for(unsigned shift=0;shift<32;shift+=8) {
        unsigned a=(actual>>shift)&255u,e=(expected>>shift)&255u;
        if(a>e+tolerance || e>a+tolerance)return 0;
    }
    return 1;
}
int ps5vk_sampler_core_case(unsigned index, struct ps5vk_sampler_core_case *out)
{
    static const struct ps5vk_sampler_core_case cases[PS5VK_SAMPLER_CORE_CASES]={
        {"mirrored-repeat",VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            -0.25f,UINT32_C(0xffff0000),0,0,0,0},
        {"transparent-black-border",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            -2.0f,UINT32_C(0x00000000),0,0,0,0},
        {"opaque-black-border",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VK_BORDER_COLOR_INT_OPAQUE_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            -2.0f,UINT32_C(0xff000000),0,0,0,0},
        {"opaque-white-border",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            -2.0f,UINT32_C(0xffffffff),0,0,0,0},
        {"nearest-center-control",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            0.5f,UINT32_C(0xff000000),1,0,0,0},
        {"linear-magnification",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_NEAREST,
            0.5f,UINT32_C(0xff808080),1,0,0,0},
        {"nearest-minification-control",VK_SAMPLER_ADDRESS_MODE_REPEAT,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_NEAREST,
            0.5f,UINT32_C(0xff000000),1,1,0,0},
        {"linear-minification",VK_SAMPLER_ADDRESS_MODE_REPEAT,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_LINEAR,
            0.5f,UINT32_C(0xff808080),1,1,0,0},
        /* Each compiled witness selects exactly one case. The four source
         * texels are red, green, blue, red in row-major order. These cases
         * distinguish mirror-once from repeat and clamp-to-edge on both axes. */
        {"mirror-u-nearest-negative",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            -0.25f,UINT32_C(0xffff0000),0,0,0.25f,1},
        {"mirror-u-nearest-inside",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            0.25f,UINT32_C(0xffff0000),0,0,0.25f,1},
        {"mirror-u-nearest-positive",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            1.25f,UINT32_C(0xff00ff00),0,0,0.25f,1},
        {"mirror-u-linear-negative",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_LINEAR,
            -0.375f,UINT32_C(0xffbf4000),0,0,0.25f,1},
        {"mirror-u-linear-inside",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_LINEAR,
            0.375f,UINT32_C(0xffbf4000),0,0,0.25f,1},
        {"mirror-u-linear-positive",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_LINEAR,
            1.25f,UINT32_C(0xff00ff00),0,0,0.25f,1},
        {"mirror-v-nearest-negative",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            0.25f,UINT32_C(0xffff0000),0,0,-0.25f,2},
        {"mirror-v-nearest-inside",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            0.25f,UINT32_C(0xffff0000),0,0,0.25f,2},
        {"mirror-v-nearest-positive",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            0.25f,UINT32_C(0xff0000ff),0,0,1.25f,2},
        {"mirror-v-linear-negative",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_LINEAR,
            0.25f,UINT32_C(0xffbf0040),0,0,-0.375f,2},
        {"mirror-v-linear-inside",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_LINEAR,
            0.25f,UINT32_C(0xffbf0040),0,0,0.375f,2},
        {"mirror-v-linear-positive",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_LINEAR,
            0.25f,UINT32_C(0xff0000ff),0,0,1.25f,2},
        /* The 3D source has two uniform 2x2 slices: red at W=0, green at
         * W=1. U and V stay at 0.25, so only W can change the output. */
        {"mirror-w-nearest-negative",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            -0.25f,UINT32_C(0xffff0000),0,0,0.25f,3},
        {"mirror-w-nearest-inside",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            0.25f,UINT32_C(0xffff0000),0,0,0.25f,3},
        {"mirror-w-nearest-positive",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            1.25f,UINT32_C(0xff00ff00),0,0,0.25f,3},
        {"mirror-w-linear-negative",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_LINEAR,
            -0.375f,UINT32_C(0xffbf4000),0,0,0.25f,3},
        {"mirror-w-linear-inside",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_LINEAR,
            0.375f,UINT32_C(0xffbf4000),0,0,0.25f,3},
        {"mirror-w-linear-positive",VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_LINEAR,
            1.25f,UINT32_C(0xff00ff00),0,0,0.25f,3},
    };
    if(!out || index>=PS5VK_SAMPLER_CORE_CASES)return -1;
    *out=cases[index];return 0;
}
