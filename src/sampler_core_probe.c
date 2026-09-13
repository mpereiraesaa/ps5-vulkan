/* Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sampler_core_probe.h"
int ps5vk_sampler_core_case(unsigned index, struct ps5vk_sampler_core_case *out)
{
    static const struct ps5vk_sampler_core_case cases[PS5VK_SAMPLER_CORE_CASES]={
        {"mirrored-repeat",VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,-0.25f,UINT32_C(0xffff0000)},
        {"transparent-black-border",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,-2.0f,UINT32_C(0x00000000)},
        {"opaque-black-border",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VK_BORDER_COLOR_INT_OPAQUE_BLACK,-2.0f,UINT32_C(0xff000000)},
        {"opaque-white-border",VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,-2.0f,UINT32_C(0xffffffff)},
    };
    if(!out || index>=PS5VK_SAMPLER_CORE_CASES)return -1;
    *out=cases[index];return 0;
}
