#ifndef PS5VK_RUNTIME_SHADER_STORAGE_H
#define PS5VK_RUNTIME_SHADER_STORAGE_H
#include "ps5_shader_header.h"
enum { PS5VK_RUNTIME_CX_MAX=16,PS5VK_RUNTIME_SH_MAX=8,PS5VK_RUNTIME_SEMANTICS_MAX=32 };
struct ps5vk_runtime_shader {
    struct ps5_shader_header header;
    struct ps5_shader_user_data resources;
    struct ps5_shader_specials specials;
    ps5_agc_register context[PS5VK_RUNTIME_CX_MAX];
    ps5_agc_register shader[PS5VK_RUNTIME_SH_MAX];
    uint32_t inputs[PS5VK_RUNTIME_SEMANTICS_MAX];
    uint32_t outputs[PS5VK_RUNTIME_SEMANTICS_MAX];
};
#endif
