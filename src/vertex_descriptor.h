#ifndef PS5VK_VERTEX_DESCRIPTOR_H
#define PS5VK_VERTEX_DESCRIPTOR_H
#include <stdint.h>
/* Nonzero byte-granular stride and native SRD-aligned base address with complete typed records.
 * Divisors and zero stride are not supported. bytes is the accessible span
 * after applying the bound buffer offset; attribute_extent is the greatest
 * attribute offset plus its format size, and may exceed the stride.
 * GFX1013 ignores the descriptor's two low address bits; native submission
 * stages a Vulkan binding with an unaligned base before calling this helper.
 * Failure leaves all four output words unchanged. */
int ps5vk_vertex_descriptor(uint32_t out[4],uint64_t address,uint64_t bytes,
                            uint32_t stride,uint32_t attribute_extent);
/* LLPC reconstructs the vertex table's high address from the shader PC. */
int ps5vk_vertex_table_address(uint64_t shader_address,uint64_t table_address,
                              uint32_t table_bytes);
#endif
