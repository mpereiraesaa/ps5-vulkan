#ifndef PS5VK_VERTEX_DESCRIPTOR_H
#define PS5VK_VERTEX_DESCRIPTOR_H
#include <stdint.h>
/* Initial float-interleaved fetch profile: four-byte aligned nonzero stride,
 * full records only. Not arbitrary Vulkan formats, divisors or zero stride.
 * bytes is the accessible span after applying the bound buffer offset.
 * Failure leaves all four output words unchanged. */
int ps5vk_vertex_descriptor(uint32_t out[4],uint64_t address,uint64_t bytes,
                            uint32_t stride,uint32_t attribute_extent);
/* LLPC reconstructs the vertex table's high address from the shader PC. */
int ps5vk_vertex_table_address(uint64_t shader_address,uint64_t table_address,
                              uint32_t table_bytes);
#endif
