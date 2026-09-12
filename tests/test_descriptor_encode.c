#include "descriptor_encode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Isolated address-resolution fixture. Actual binding lifetime/range validation
 * is tested by test_vk_memory; this test is not evidence of GPU mappings. */
VkResult ps5vk_buffer_span(VkDevice d, VkBuffer b, VkDeviceSize offset,
                          VkDeviceSize range, void **address, VkDeviceSize *size)
{
    (void)d;
    if (!b) return VK_ERROR_UNKNOWN;
    *address = (void *)((uintptr_t)b + offset); *size = range;
    return VK_SUCCESS;
}
int main(void)
{
    struct VkDevice_T device = {0};
    struct VkDescriptorPool_T pool = {.device = &device};
    struct VkDescriptorSet_T set = {.pool = &pool};
    set.signature.binding[0] = (struct ps5vk_binding){1, 0, VK_SHADER_STAGE_COMPUTE_BIT};
    set.signature.binding[1] = (struct ps5vk_binding){1, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    set.defined[0] = set.defined[1] = VK_TRUE;
    set.buffers[0] = (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x100004000, 128, 4096};
    set.buffers[1] = (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x200006000, 256, 2048};
    struct ps5vk_compiled_program p = {.gfx = 1013, .descriptor_count = 2,
        .descriptors = {{0, 1, 0, 0}, {0, 0, 0, 8}}};
    uint32_t table[16], saved[16]; memset(table, 0xab, sizeof(table));
    assert(ps5vk_descriptor_encode(&device, &p, &set, table, 16) == VK_SUCCESS);
    assert(table[0] == 0x6100 && table[1] == 2 && table[2] == 2048);
    assert(table[8] == 0x4080 && table[9] == 1 && table[10] == 4096);
    assert(table[3] == 0x31016fac && table[11] == 0x31016fac);
    assert(!table[4] && !table[7] && table[12] == 0xabababab);
    memcpy(saved, table, sizeof(table));
    set.signature.binding[0].count = 2;
    assert(ps5vk_descriptor_encode(&device, &p, &set, table, 16) == VK_SUCCESS);
    assert(!memcmp(saved, table, sizeof(table))); /* Scalar shader uses element zero. */
    set.signature.binding[0].count = 1;
    assert(ps5vk_descriptor_encode(&device, &p, &set, table, 11) != VK_SUCCESS);
    set.defined[0] = VK_FALSE;
    assert(ps5vk_descriptor_encode(&device, &p, &set, table, 16) != VK_SUCCESS);
    set.defined[0] = VK_TRUE; set.buffers[0].range = UINT64_C(1) << 32;
    assert(ps5vk_descriptor_encode(&device, &p, &set, table, 16) != VK_SUCCESS);
    set.buffers[0].range = 4096; p.descriptors[1].table_dword = 0;
    assert(ps5vk_descriptor_encode(&device, &p, &set, table, 16) != VK_SUCCESS);
    assert(!memcmp(saved, table, sizeof(table)));
    puts("Compiler-ordered raw descriptor table: pass (host only)");
}
