#ifndef PS5VK_SPIRV_GRAPHICS_INTERFACE_H
#define PS5VK_SPIRV_GRAPHICS_INTERFACE_H
#include "graphics_program.h"
/* Profile validation, NOT a complete SPIR-V validator. Accepts smooth float32
 * scalar/vector varyings at whole locations; rejects unsupported interfaces. */
int ps5vk_spirv_graphics_interface(const struct ps5vk_graphics_key *);
#endif
