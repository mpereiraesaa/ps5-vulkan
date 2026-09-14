/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sampler_core_probe.h"
int ps5vk_sampler_core_case(unsigned index, struct ps5vk_sampler_core_case *out)
{
    static const struct ps5vk_sampler_core_case cases[PS5VK_SAMPLER_CORE_CASES]={
        {"mirrored-repeat",VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            -0.25f,UINT32_C(0xffff0000),0,0},
        {"transparent-black-border",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            -2.0f,UINT32_C(0x00000000),0,0},
        {"opaque-black-border",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VK_BORDER_COLOR_INT_OPAQUE_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            -2.0f,UINT32_C(0xff000000),0,0},
        {"opaque-white-border",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            -2.0f,UINT32_C(0xffffffff),0,0},
        {"nearest-center-control",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_NEAREST,
            0.5f,UINT32_C(0xff000000),1,0},
        {"linear-magnification",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_NEAREST,
            0.5f,UINT32_C(0xff808080),1,0},
        {"nearest-minification-control",VK_SAMPLER_ADDRESS_MODE_REPEAT,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_LINEAR,VK_FILTER_NEAREST,
            0.5f,UINT32_C(0xff000000),1,1},
        {"linear-minification",VK_SAMPLER_ADDRESS_MODE_REPEAT,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_FILTER_NEAREST,VK_FILTER_LINEAR,
            0.5f,UINT32_C(0xff808080),1,1},
    };
    if(!out || index>=PS5VK_SAMPLER_CORE_CASES)return -1;
    *out=cases[index];return 0;
}
