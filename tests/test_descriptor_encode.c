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
    set.signature.type[0]=set.signature.type[1]=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    set.defined[0] = set.defined[1] = VK_TRUE;
    set.buffers[0] = (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x100004000, 128, 4096};
    set.buffers[1] = (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x200006000, 256, 2048};
    struct ps5vk_compiled_program p = {.gfx = 1013, .descriptor_count = 2,
        .descriptors = {{0, 1, 0, 0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                        {0, 0, 0, 8,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}};
    uint32_t table[16], saved[16]; memset(table, 0xab, sizeof(table));
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, table, 16) == VK_SUCCESS);
    assert(table[0] == 0x6100 && table[1] == 2 && table[2] == 2048);
    assert(table[8] == 0x4080 && table[9] == 1 && table[10] == 4096);
    assert(table[3] == 0x31016fac && table[11] == 0x31016fac);
    assert(!table[4] && !table[7] && table[12] == 0xabababab);
    memcpy(saved, table, sizeof(table));
    set.signature.binding[0].count = 2;
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, table, 16) == VK_SUCCESS);
    assert(!memcmp(saved, table, sizeof(table))); /* Scalar shader uses element zero. */
    set.signature.binding[0].count = 1;
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, table, 11) != VK_SUCCESS);
    set.defined[0] = VK_FALSE;
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, table, 16) != VK_SUCCESS);
    set.defined[0] = VK_TRUE; set.buffers[0].range = UINT64_C(1) << 32;
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, table, 16) != VK_SUCCESS);
    set.buffers[0].range = 4096; p.descriptors[1].table_dword = 0;
    assert(ps5vk_descriptor_encode(&device, &p, 0, &set, table, 16) != VK_SUCCESS);
    assert(!memcmp(saved, table, sizeof(table)));
    struct VkBufferView_T view={.device=&device,.buffer=(VkBuffer)(uintptr_t)0x200004000,
        .format=VK_FORMAT_R32_UINT,.offset=64,.range=256};
    struct VkDescriptorSet_T texel={.pool=&pool,.defined={VK_TRUE},.texel_views={&view}};
    texel.signature.binding[0]=(struct ps5vk_binding){1,0,VK_SHADER_STAGE_COMPUTE_BIT};
    texel.signature.type[0]=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    struct ps5vk_compiled_program typed={.gfx=1013,.descriptor_count=1,
        .descriptors={{1,0,0,0,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER}}};
    memset(table,0,sizeof(table));
    assert(ps5vk_descriptor_encode(&device,&typed,1,&texel,table,16)==VK_SUCCESS);
    assert(table[0]==0x4040 && table[1]==2 && table[2]==64 && table[3]==0x31014fac);
    puts("Compiler-ordered raw descriptor table: pass (host only)");
}
