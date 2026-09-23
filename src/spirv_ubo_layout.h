#ifndef PS5VK_SPIRV_UBO_LAYOUT_H
#define PS5VK_SPIRV_UBO_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

/* Validate Block-decorated Uniform variables against Vulkan's UBO block
 * layout rules. standard_layout selects base rather than extended alignment.
 * Unknown layouts in a referenced UBO fail closed; unrelated storage classes
 * are not interpreted here. */
int ps5vk_spirv_validate_ubo_layout(const uint32_t *words, size_t count,
                                    int standard_layout);

#endif
